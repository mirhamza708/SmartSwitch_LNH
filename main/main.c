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

#if IP_NAPT
#include "lwip/lwip_napt.h"
#endif

#include "lwip/err.h"
#include "lwip/sys.h"
#include "driver/gpio.h"
#include "hardware_definitions.h"

#define ESP_INTR_FLAG_DEFAULT 0

/* STA/AP Configuration (Using your existing Logic) */
#define EXAMPLE_ESP_WIFI_STA_SSID       CONFIG_ESP_WIFI_REMOTE_AP_SSID
#define EXAMPLE_ESP_WIFI_STA_PASSWD     CONFIG_ESP_WIFI_REMOTE_AP_PASSWORD
#define EXAMPLE_ESP_MAXIMUM_RETRY       CONFIG_ESP_MAXIMUM_STA_RETRY

#if CONFIG_ESP_WIFI_AUTH_OPEN
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD   WIFI_AUTH_OPEN
#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD   WIFI_AUTH_WPA2_PSK
#endif

#define EXAMPLE_ESP_WIFI_AP_SSID        CONFIG_ESP_WIFI_AP_SSID
#define EXAMPLE_ESP_WIFI_AP_PASSWD      CONFIG_ESP_WIFI_AP_PASSWORD
#define EXAMPLE_ESP_WIFI_CHANNEL       CONFIG_ESP_WIFI_AP_CHANNEL
#define EXAMPLE_MAX_STA_CONN            CONFIG_ESP_MAX_STA_CONN_AP

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define DHCPS_OFFER_DNS    0x02

static const char *TAG_AP = "WiFi SoftAP";
static const char *TAG_STA = "WiFi Sta";
static int s_retry_num = 0;
static EventGroupHandle_t s_wifi_event_group;

// --- WEB SERVER LOGIC START ---

/* Simple HTML page with a button */
const char* html_page = 
"<html><head><title>ESP32 Controller</title></head>"
"<body><h1>ESP32 Web Server</h1>"
"<p>Status LED Control:</p>"
"<button onclick=\"fetch('/toggle')\">TOGGLE LED</button>"
"</body></html>";

/* Handler for the Main Page (/) */
esp_err_t get_handler(httpd_req_t *req) {
    httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
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

/* Start the Web Server */
void start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_root = {.uri = "/", .method = HTTP_GET, .handler = get_handler};
        httpd_uri_t uri_toggle = {.uri = "/toggle", .method = HTTP_GET, .handler = toggle_handler};
        httpd_register_uri_handler(server, &uri_root);
        httpd_register_uri_handler(server, &uri_toggle);
        ESP_LOGI("SERVER", "Web server started!");
    }
}
// --- WEB SERVER LOGIC END ---

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *) event_data;
        ESP_LOGI(TAG_AP, "Station "MACSTR" joined", MAC2STR(event->mac));
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG_STA, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_netif_t *wifi_init_softap(void) {
    esp_netif_t *esp_netif_ap = esp_netif_create_default_wifi_ap();
    wifi_config_t wifi_ap_config = {
        .ap = {
            .ssid = EXAMPLE_ESP_WIFI_AP_SSID,
            .ssid_len = strlen(EXAMPLE_ESP_WIFI_AP_SSID),
            .channel = EXAMPLE_ESP_WIFI_CHANNEL,
            .password = EXAMPLE_ESP_WIFI_AP_PASSWD,
            .max_connection = EXAMPLE_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));
    return esp_netif_ap;
}

esp_netif_t *wifi_init_sta(void) {
    esp_netif_t *esp_netif_sta = esp_netif_create_default_wifi_sta();
    wifi_config_t wifi_sta_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_STA_SSID,
            .password = EXAMPLE_ESP_WIFI_STA_PASSWD,
            .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
        },
    };
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

void configure_gpios(void) {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = STATUS_LED_PIN_BITMASK,
        .pull_down_en = 0,
        .pull_up_en = 0
    };
    gpio_config(&io_conf);
}

void app_main(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    configure_gpios(); // Initialize the LED pin

    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    esp_netif_t *esp_netif_ap = wifi_init_softap();
    esp_netif_t *esp_netif_sta = wifi_init_sta();

    ESP_ERROR_CHECK(esp_wifi_start());

    // Wait for connection
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    softap_set_dns_addr(esp_netif_ap, esp_netif_sta);
    esp_netif_set_default_netif(esp_netif_sta);

    // Enable NAPT for the hotspot
    esp_netif_napt_enable(esp_netif_ap);

    // START THE WEB SERVER
    start_webserver();
}