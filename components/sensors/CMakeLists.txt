idf_component_register(SRCS "sensors.c" "sensors_util.c" "drv_vl53l1x.c"
                       INCLUDE_DIRS "include"
                       REQUIRES sa_config sa_i2c
                       PRIV_REQUIRES esp_driver_i2c esp_driver_gpio esp_rom esp_timer nvs_flash log sa_mqtt)
