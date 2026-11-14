idf_component_register(SRCS "app_main.c" "console.c"
                       INCLUDE_DIRS "."
                       REQUIRES rules sa_config sa_i2c sa_log sa_mqtt sensors actors wifi_mgr webui
                                nvs_flash esp_netif esp_event esp_timer esp_app_format app_update console esp_wifi lwip)
