idf_component_register(SRCS "sa_i2c.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_i2c log sa_config)
