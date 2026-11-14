idf_component_register(SRCS "actors.c" "actors_util.c"
                       INCLUDE_DIRS "include"
                       REQUIRES rules sa_config sensors
                       PRIV_REQUIRES esp_driver_gpio esp_timer nvs_flash log sa_mqtt)
