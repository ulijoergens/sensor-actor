idf_component_register(SRCS "wifi_mgr.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_wifi esp_netif esp_event esp_timer esp_hw_support log sa_config sa_captive)
