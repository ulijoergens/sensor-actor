idf_component_register(SRCS "sa_captive.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_server
                       PRIV_REQUIRES lwip esp_netif log)
