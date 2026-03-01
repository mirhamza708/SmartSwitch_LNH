idf_component_register(SRCS "main.c"
                    PRIV_REQUIRES esp_wifi nvs_flash driver esp_http_server spiffs app_update
                    INCLUDE_DIRS ".")
