#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_net_stack.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "esp_http_server.h" // Added for the Web Server
#include "nvs.h" // Added for NVS storage of WiFi credentials

#if IP_NAPT
#include "lwip/lwip_napt.h"
#endif

#include "lwip/err.h"
#include "lwip/sys.h"
#include "driver/gpio.h"
#include "hardware_definitions.h"
#include "esp_spiffs.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

#define ESP_INTR_FLAG_DEFAULT 0

/* The examples use WiFi configuration that you can set via project configuration menu.

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_ESP_WIFI_STA_SSID "mywifissid"
*/

/* STA Configuration */
#define EXAMPLE_ESP_WIFI_STA_SSID           CONFIG_ESP_WIFI_REMOTE_AP_SSID
#define EXAMPLE_ESP_WIFI_STA_PASSWD         CONFIG_ESP_WIFI_REMOTE_AP_PASSWORD
#define EXAMPLE_ESP_MAXIMUM_RETRY           CONFIG_ESP_MAXIMUM_STA_RETRY

#if CONFIG_ESP_WIFI_AUTH_OPEN
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD   WIFI_AUTH_OPEN
#elif CONFIG_ESP_WIFI_AUTH_WEP
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD   WIFI_AUTH_WEP
#elif CONFIG_ESP_WIFI_AUTH_WPA_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD   WIFI_AUTH_WPA_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD   WIFI_AUTH_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD   WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD   WIFI_AUTH_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD   WIFI_AUTH_WPA2_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WAPI_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD   WIFI_AUTH_WAPI_PSK
#endif

/* AP Configuration */
#define EXAMPLE_ESP_WIFI_AP_SSID            CONFIG_ESP_WIFI_AP_SSID
#define EXAMPLE_ESP_WIFI_AP_PASSWD          CONFIG_ESP_WIFI_AP_PASSWORD
#define EXAMPLE_ESP_WIFI_CHANNEL            CONFIG_ESP_WIFI_AP_CHANNEL
#define EXAMPLE_MAX_STA_CONN                CONFIG_ESP_MAX_STA_CONN_AP


/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

/*DHCP server option*/
#define DHCPS_OFFER_DNS             0x02

static const char *TAG_AP = "WiFi SoftAP";
static const char *TAG_STA = "WiFi Sta";

static int s_retry_num = 0;

/* FreeRTOS event group to signal when we are connected/disconnected */
static EventGroupHandle_t s_wifi_event_group;

static TaskHandle_t led_task_handle = NULL;
static TaskHandle_t schedule_task_handle = NULL;
static QueueHandle_t led_queue;

typedef enum {
    LED_MODE_OFF,
    LED_MODE_ON,
    LED_MODE_BLINK
} led_mode_t;

typedef struct {
    led_mode_t mode;
    uint32_t on_time_ms;
    uint32_t off_time_ms;
} led_cmd_t;


//wifi section
 #define STORAGE_NAMESPACE "storage"

void save_wifi_credentials(const char* ssid, const char* pass) {
    nvs_handle_t my_handle;
    if (nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_set_str(my_handle, "ssid", ssid);
        nvs_set_str(my_handle, "pass", pass);
        nvs_commit(my_handle);
        nvs_close(my_handle);
    }
}

/* --- Scheduling Globals --- */
static bool time_is_set = false;
static bool is_scheduled = false;
static int sched_hour = -1;
static int sched_minute = -1;
static int sched_action = 0; 
static int sched_duration = 0;      // In minutes
static uint8_t sched_days_mask = 0; // Bitmask for days (1=Sun, 2=Mon... 64=Sat). 0 = Run Once

// Prevent re-triggering multiple times in the same minute
static int last_trigger_day = -1;
static int last_trigger_min = -1;

// Variables to track the "Turn Off After Duration" state
static bool duration_active = false;
static time_t duration_end_time = 0;
static int duration_revert_action = 0;

esp_err_t get_handler(httpd_req_t *req)
{
    FILE* f = fopen("/spiffs/index.html", "r");
    if (!f) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    char buffer[1024];
    size_t read_bytes;

    while ((read_bytes = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        httpd_resp_send_chunk(req, buffer, read_bytes);
    }

    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);  // End response
    return ESP_OK;
}

/* Handler to Toggle the LED (/toggle) */
esp_err_t toggle_handler(httpd_req_t *req) {
    static int led_state = 0;
    led_state = !led_state;
    gpio_set_level(STATUS_LED_PIN, led_state);
    ESP_LOGI("SERVER", "LED Toggled to %d", led_state);
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t set_time_handler(httpd_req_t *req) {
    char buf[50];
    char h[5], m[5];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        if (httpd_query_key_value(buf, "h", h, sizeof(h)) == ESP_OK &&
            httpd_query_key_value(buf, "m", m, sizeof(m)) == ESP_OK) {
            struct tm tm = { .tm_hour = atoi(h), .tm_min = atoi(m), .tm_year = 126, .tm_mon = 0, .tm_mday = 1 };
            time_t t = mktime(&tm);
            struct timeval now = { .tv_sec = t };
            settimeofday(&now, NULL);
            time_is_set = true;
            ESP_LOGI("TIME", "Manual sync: %02d:%02d", tm.tm_hour, tm.tm_min);
        }
    }
    httpd_resp_send(req, "Time Set", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t schedule_handler(httpd_req_t *req) {
    char buf[100];
    char h[5], m[5], a[5], dur[10], d[5];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        if (httpd_query_key_value(buf, "h", h, sizeof(h)) == ESP_OK &&
            httpd_query_key_value(buf, "m", m, sizeof(m)) == ESP_OK &&
            httpd_query_key_value(buf, "a", a, sizeof(a)) == ESP_OK &&
            httpd_query_key_value(buf, "dur", dur, sizeof(dur)) == ESP_OK &&
            httpd_query_key_value(buf, "d", d, sizeof(d)) == ESP_OK) {
            
            sched_hour = atoi(h);
            sched_minute = atoi(m);
            sched_action = atoi(a);
            sched_duration = atoi(dur);
            sched_days_mask = atoi(d);
            
            is_scheduled = true;
            last_trigger_day = -1; // Reset anti-spam flags
            last_trigger_min = -1;
            duration_active = false; // Cancel any ongoing duration timers

            ESP_LOGI("SCHED", "Task set: %02d:%02d Act:%d Dur:%d mins DaysMask:%d", 
                     sched_hour, sched_minute, sched_action, sched_duration, sched_days_mask);
        }
    }
    httpd_resp_send(req, "Scheduled", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// HTTP handler
esp_err_t config_wifi_handler(httpd_req_t *req) {
    char buf[128];
    char ssid[32], pass[64];

    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        if (httpd_query_key_value(buf, "ssid", ssid, sizeof(ssid)) == ESP_OK &&
            httpd_query_key_value(buf, "pass", pass, sizeof(pass)) == ESP_OK) {
            
            ESP_LOGI("WIFI", "New Creds: %s", ssid);
            save_wifi_credentials(ssid, pass);

            wifi_config_t wifi_config = {
                .sta = { .threshold.authmode = WIFI_AUTH_WPA2_PSK },
            };
            strncpy((char*)wifi_config.sta.ssid, ssid, 32);
            strncpy((char*)wifi_config.sta.password, pass, 64);

            esp_wifi_disconnect();
            esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
            esp_wifi_connect();

            httpd_resp_send(req, "Connecting... checking status.", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
    }
    httpd_resp_send_404(req);
    return ESP_FAIL;
}

esp_err_t get_ip_handler(httpd_req_t *req) {
    esp_netif_ip_info_t ip_info;
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_get_ip_info(netif, &ip_info);
    
    char ip_str[16];
    esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
    httpd_resp_send(req, ip_str, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
// --- BACKGROUND TASK ---
void schedule_monitor_task(void *pv) {
    while(1) {
        if (time_is_set) {
            time_t now;
            struct tm ti;
            time(&now);
            localtime_r(&now, &ti);

            // 1. Check if a duration timer has expired
            if (duration_active && now >= duration_end_time) {
                gpio_set_level(STATUS_LED_PIN, duration_revert_action);
                duration_active = false; // Turn off the timer flag
                ESP_LOGI("TASK", "Duration Ended! Reverting state to: %d", duration_revert_action);
            }

            // 2. Check if it's time to trigger the main schedule
            if (is_scheduled) {
                bool day_match = false;
                
                // If mask is 0, user didn't pick any days. Treat it as a "Run Once" today/tomorrow.
                if (sched_days_mask == 0) {
                    day_match = true; 
                } else {
                    // Check if today's bit is set in the mask (ti.tm_wday: 0=Sun, 1=Mon, etc)
                    day_match = (sched_days_mask & (1 << ti.tm_wday)) != 0;
                }

                if (day_match && ti.tm_hour == sched_hour && ti.tm_min == sched_minute) {
                    // Prevent triggering 60 times in the same minute
                    if (last_trigger_day != ti.tm_yday || last_trigger_min != ti.tm_min) {
                        
                        // Execute Action
                        gpio_set_level(STATUS_LED_PIN, sched_action);
                        ESP_LOGI("TASK", "Schedule Fired! State: %d", sched_action);

                        // If a duration is set, start the countdown clock
                        if (sched_duration > 0) {
                            duration_end_time = now + (sched_duration * 60); // mins to seconds
                            duration_revert_action = !sched_action; // Opposite action
                            duration_active = true;
                            ESP_LOGI("TASK", "Duration set! Will revert in %d mins", sched_duration);
                        }

                        // Mark this minute as triggered
                        last_trigger_day = ti.tm_yday;
                        last_trigger_min = ti.tm_min;

                        // If it's a "Run Once" schedule (no days selected), turn it off now
                        if (sched_days_mask == 0) {
                            is_scheduled = false;
                        }
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000)); // Check every 1 second to keep duration timing accurate
    }
}

void led_task(void *arg)
{
    led_cmd_t current_cmd = {
        .mode = LED_MODE_OFF,
        .on_time_ms = 0,
        .off_time_ms = 0
    };
    gpio_set_level(STATUS_LED_PIN, 0);

    while (1) {

        // Check if new command arrived (non-blocking)
        led_cmd_t new_cmd;
        if (xQueueReceive(led_queue, &new_cmd, pdMS_TO_TICKS(5)) == pdTRUE) {
            ESP_LOGI("LEDTASK", "CMD %d", new_cmd.mode);
            current_cmd = new_cmd;
        }

        switch (current_cmd.mode) {

            case LED_MODE_OFF:
                gpio_set_level(STATUS_LED_PIN, 0);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;

            case LED_MODE_ON:
                gpio_set_level(STATUS_LED_PIN, 1);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;

            case LED_MODE_BLINK:
                gpio_set_level(STATUS_LED_PIN, 1);
                vTaskDelay(pdMS_TO_TICKS(current_cmd.on_time_ms));

                gpio_set_level(STATUS_LED_PIN, 0);
                vTaskDelay(pdMS_TO_TICKS(current_cmd.off_time_ms));
                break;
        }
    }
}

esp_err_t ota_post_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    esp_ota_handle_t ota_handle = 0;
    bool ota_started = false;

    // Start upload blink
    led_cmd_t upload_blink = {
        .mode = LED_MODE_BLINK,
        .on_time_ms = 100,
        .off_time_ms = 100
    };
    xQueueSend(led_queue, &upload_blink, pdMS_TO_TICKS(10));

    const esp_partition_t *ota_partition =
        esp_ota_get_next_update_partition(NULL);

    if (!ota_partition) {
        httpd_resp_sendstr(req, "No OTA partition found");
        ret = ESP_FAIL;
        goto cleanup;
    }

    if (esp_ota_begin(ota_partition, OTA_SIZE_UNKNOWN, &ota_handle) != ESP_OK) {
        httpd_resp_sendstr(req, "OTA Begin Failed");
        ret = ESP_FAIL;
        goto cleanup;
    }

    ota_started = true;

    char buffer[1024];
    int remaining = req->content_len;

    while (remaining > 0) {

        int recv_len = httpd_req_recv(
            req,
            buffer,
            remaining > sizeof(buffer) ? sizeof(buffer) : remaining
        );

        if (recv_len <= 0) {
            ret = ESP_FAIL;
            goto cleanup;
        }

        if (esp_ota_write(ota_handle, buffer, recv_len) != ESP_OK) {
            ret = ESP_FAIL;
            goto cleanup;
        }

        remaining -= recv_len;
    }

    if (esp_ota_end(ota_handle) != ESP_OK) {
        httpd_resp_sendstr(req, "OTA End Failed");
        ret = ESP_FAIL;
        goto cleanup;
    }

    ota_started = false;  // Already ended successfully

    if (esp_ota_set_boot_partition(ota_partition) != ESP_OK) {
        httpd_resp_sendstr(req, "Set Boot Partition Failed");
        ret = ESP_FAIL;
        goto cleanup;
    }

    httpd_resp_sendstr(req, "Firmware updated! Rebooting...");

cleanup:

    if (ret == ESP_OK) {

        // Stop blinking before reboot
        led_cmd_t led_off = { .mode = LED_MODE_OFF };
        xQueueSend(led_queue, &led_off, pdMS_TO_TICKS(10));
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();

    } else {

        // If OTA started but failed, abort properly
        if (ota_started) {
            esp_ota_end(ota_handle);
        }

        // Error blink pattern (slow blink)
        led_cmd_t error_blink = {
            .mode = LED_MODE_BLINK,
            .on_time_ms = 500,
            .off_time_ms = 500
        };
        xQueueSend(led_queue, &error_blink, pdMS_TO_TICKS(10));
    }

    return ret;
}

esp_err_t spiffs_post_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;

    led_cmd_t upload_blink = {
        .mode = LED_MODE_BLINK,
        .on_time_ms = 100,
        .off_time_ms = 100
    };
    xQueueSend(led_queue, &upload_blink, pdMS_TO_TICKS(10));

    const esp_partition_t *spiffs_partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                 ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
                                 NULL);

    if (!spiffs_partition) {
        httpd_resp_sendstr(req, "SPIFFS partition not found");
        ret = ESP_FAIL;
        goto cleanup;
    }

    esp_vfs_spiffs_unregister(NULL);

    if (esp_partition_erase_range(spiffs_partition, 0,
                                  spiffs_partition->size) != ESP_OK) {
        httpd_resp_sendstr(req, "SPIFFS erase failed");
        ret = ESP_FAIL;
        goto cleanup;
    }

    char buffer[1024];
    int remaining = req->content_len;
    int offset = 0;

    while (remaining > 0) {

        int recv_len = httpd_req_recv(
            req,
            buffer,
            remaining > sizeof(buffer) ? sizeof(buffer) : remaining
        );

        if (recv_len <= 0) {
            ret = ESP_FAIL;
            goto cleanup;
        }

        if (esp_partition_write(spiffs_partition,
                                offset,
                                buffer,
                                recv_len) != ESP_OK) {
            httpd_resp_sendstr(req, "SPIFFS write failed");
            ret = ESP_FAIL;
            goto cleanup;
        }

        offset += recv_len;
        remaining -= recv_len;
    }

    httpd_resp_sendstr(req, "SPIFFS updated! Rebooting...");

cleanup:

    if (ret == ESP_OK) {
        led_cmd_t led_off = { .mode = LED_MODE_OFF };
        xQueueSend(led_queue, &led_off, pdMS_TO_TICKS(10));
        ESP_LOGI("SERVER", "SPIFFS updated successfully.");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else {
        // Blink slow to indicate error
        led_cmd_t error_blink = {
            .mode = LED_MODE_BLINK,
            .on_time_ms = 500,
            .off_time_ms = 500
        };
        xQueueSend(led_queue, &error_blink, pdMS_TO_TICKS(10));

        // Optional: remount SPIFFS so system continues working
        esp_vfs_spiffs_register(&(esp_vfs_spiffs_conf_t){
            .base_path = "/spiffs",
            .partition_label = NULL,
            .max_files = 5,
            .format_if_mount_failed = false
        });
    }

    return ret;
}

/* Start the Web Server */
void start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    // We must start the server FIRST so that 'server' is no longer NULL
    if (httpd_start(&server, &config) == ESP_OK) {
        // Now define all URIs
        httpd_uri_t uri_root = {.uri = "/", .method = HTTP_GET, .handler = get_handler};
        httpd_uri_t uri_toggle = {.uri = "/toggle", .method = HTTP_GET, .handler = toggle_handler};
        httpd_uri_t ota_uri = {.uri = "/update", .method = HTTP_POST, .handler = ota_post_handler, .user_ctx = NULL};
        httpd_uri_t spiffs_uri = {.uri = "/update_spiffs", .method = HTTP_POST, .handler = spiffs_post_handler, .user_ctx = NULL};
        httpd_uri_t uri_tim = {.uri = "/set_time", .method = HTTP_GET, .handler = set_time_handler};
        httpd_uri_t uri_sch = {.uri = "/schedule", .method = HTTP_GET, .handler = schedule_handler};
        
        // New Wi-Fi Config URIs
        httpd_uri_t uri_wifi = {.uri = "/config_wifi", .method = HTTP_GET, .handler = config_wifi_handler};
        httpd_uri_t uri_ip = {.uri = "/get_ip", .method = HTTP_GET, .handler = get_ip_handler};
        
        // Register everything to the valid 'server' handle
        httpd_register_uri_handler(server, &uri_root);
        httpd_register_uri_handler(server, &uri_toggle);
        httpd_register_uri_handler(server, &ota_uri);
        httpd_register_uri_handler(server, &spiffs_uri);
        httpd_register_uri_handler(server, &uri_tim);
        httpd_register_uri_handler(server, &uri_sch);
        
        // Register the new ones
        httpd_register_uri_handler(server, &uri_wifi);
        httpd_register_uri_handler(server, &uri_ip);

        ESP_LOGI("SERVER", "Web server started successfully with all handlers!");
    } else {
        ESP_LOGE("SERVER", "Failed to start web server!");
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *) event_data;
        ESP_LOGI(TAG_AP, "Station "MACSTR" joined, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *) event_data;
        ESP_LOGI(TAG_AP, "Station "MACSTR" left, AID=%d, reason:%d",
                 MAC2STR(event->mac), event->aid, event->reason);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        ESP_LOGI(TAG_STA, "Station started");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG_STA, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* Initialize soft AP */
esp_netif_t *wifi_init_softap(void)
{
    esp_netif_t *esp_netif_ap = esp_netif_create_default_wifi_ap();

    wifi_config_t wifi_ap_config = {
        .ap = {
            .ssid = EXAMPLE_ESP_WIFI_AP_SSID,
            .ssid_len = strlen(EXAMPLE_ESP_WIFI_AP_SSID),
            .channel = EXAMPLE_ESP_WIFI_CHANNEL,
            .password = EXAMPLE_ESP_WIFI_AP_PASSWD,
            .max_connection = EXAMPLE_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
        },
    };

    if (strlen(EXAMPLE_ESP_WIFI_AP_PASSWD) == 0) {
        wifi_ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));

    ESP_LOGI(TAG_AP, "wifi_init_softap finished. SSID:%s password:%s channel:%d",
             EXAMPLE_ESP_WIFI_AP_SSID, EXAMPLE_ESP_WIFI_AP_PASSWD, EXAMPLE_ESP_WIFI_CHANNEL);

    return esp_netif_ap;
}

/* Initialize wifi station */
esp_netif_t *wifi_init_sta(void) {
    esp_netif_t *esp_netif_sta = esp_netif_create_default_wifi_sta();
    wifi_config_t wifi_sta_config = { .sta = { .threshold.authmode = WIFI_AUTH_WPA2_PSK } };

    nvs_handle_t my_handle;
    size_t ssid_len = 32, pass_len = 64;
    // Load from NVS
    if (nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &my_handle) == ESP_OK) {
        nvs_get_str(my_handle, "ssid", (char*)wifi_sta_config.sta.ssid, &ssid_len);
        nvs_get_str(my_handle, "pass", (char*)wifi_sta_config.sta.password, &pass_len);
        nvs_close(my_handle);
    } else {
        // Use your CONFIG defaults from Kconfig if nothing is saved yet
        strncpy((char*)wifi_sta_config.sta.ssid, EXAMPLE_ESP_WIFI_STA_SSID, 32);
        strncpy((char*)wifi_sta_config.sta.password, EXAMPLE_ESP_WIFI_STA_PASSWD, 64);
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config));
    return esp_netif_sta;
}


void softap_set_dns_addr(esp_netif_t *esp_netif_ap, esp_netif_t *esp_netif_sta) {
    esp_netif_dns_info_t dns;
    esp_netif_get_dns_info(esp_netif_sta, ESP_NETIF_DNS_MAIN, &dns);
    uint8_t dhcps_offer_option = DHCPS_OFFER_DNS;
    esp_netif_dhcps_stop(esp_netif_ap);
    esp_netif_dhcps_option(esp_netif_ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &dhcps_offer_option, sizeof(dhcps_offer_option));
    esp_netif_set_dns_info(esp_netif_ap, ESP_NETIF_DNS_MAIN, &dns);
    esp_netif_dhcps_start(esp_netif_ap);
}

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t)arg;
    if (gpio_num == BOOT_PIN)
    {
        gpio_set_level(STATUS_LED_PIN, gpio_get_level(BOOT_PIN));
    }
}

void configure_gpios(void)
{
    // zero-initialize the config structure.
    gpio_config_t io_conf = {};
    // disable interrupt
    io_conf.intr_type = GPIO_INTR_DISABLE;
    // set as output mode
    io_conf.mode = GPIO_MODE_OUTPUT;
    // bit mask of the pins that you want to set,e.g.GPIO18/19
    io_conf.pin_bit_mask = STATUS_LED_PIN_BITMASK;
    // disable pull-down mode
    io_conf.pull_down_en = 0;
    // disable pull-up mode
    io_conf.pull_up_en = 0;
    // configure GPIO with the given settings
    gpio_config(&io_conf);

    // disable interrupt
    io_conf.intr_type = GPIO_INTR_ANYEDGE;
    // set as output mode
    io_conf.mode = GPIO_MODE_INPUT;
    // bit mask of the pins that you want to set,e.g.GPIO18/19
    io_conf.pin_bit_mask = BOOT_PIN_BITMASK;
    // disable pull-down mode
    io_conf.pull_down_en = 0;
    // disable pull-up mode
    io_conf.pull_up_en = 1;
    // configure GPIO with the given settings
    gpio_config(&io_conf);

    // change gpio interrupt type for one pin
    gpio_set_intr_type(BOOT_PIN, GPIO_INTR_ANYEDGE);

    // install gpio isr service
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    // hook isr handler for specific gpio pin
    gpio_isr_handler_add(BOOT_PIN, gpio_isr_handler, (void *)BOOT_PIN);

    // Dump gpio configuration to terminal
    // gpio_dump_io_configuration(stdout, SOC_GPIO_VALID_GPIO_MASK);
}

void init_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        ESP_LOGE("SPIFFS", "Mount failed");
    } else {
        ESP_LOGI("SPIFFS", "Mounted successfully");
    }
}

void app_main(void) {
    
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;

    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }

    configure_gpios(); // Initialize the LED pin
    led_queue = xQueueCreate(1, sizeof(led_cmd_t));
    xTaskCreate(led_task, "led_task", 2048, NULL, 5, &led_task_handle);
    xTaskCreate(schedule_monitor_task, "schedule_task", 2048, NULL, 6, &schedule_task_handle);
    
    init_spiffs(); // mount spiffs

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);


    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    esp_netif_t *esp_netif_ap = wifi_init_softap();
    esp_netif_t *esp_netif_sta = wifi_init_sta();

    ESP_ERROR_CHECK(esp_wifi_start());
    // START THE WEB SERVER
    start_webserver();

    // Wait for connection
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    /* xEventGroupWaitBits() returns the bits before the call returned,
     * hence we can test which event actually happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG_STA, "connected to ap SSID:%s password:%s",
                 EXAMPLE_ESP_WIFI_STA_SSID, EXAMPLE_ESP_WIFI_STA_PASSWD);
        softap_set_dns_addr(esp_netif_ap,esp_netif_sta);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG_STA, "Failed to connect to SSID:%s, password:%s",
                 EXAMPLE_ESP_WIFI_STA_SSID, EXAMPLE_ESP_WIFI_STA_PASSWD);
    } else {
        ESP_LOGE(TAG_STA, "UNEXPECTED EVENT");
        return;
    }

    softap_set_dns_addr(esp_netif_ap, esp_netif_sta);
    esp_netif_set_default_netif(esp_netif_sta);

    // Enable NAPT for the hotspot
    esp_netif_napt_enable(esp_netif_ap);

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}