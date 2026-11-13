idf_component_register(SRCS "sa_mqtt.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_app_format esp_hw_support esp_timer esp_event esp_netif esp_wifi log sa_config wifi_mgr)
