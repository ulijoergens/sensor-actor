idf_component_register(SRCS "sa_i2c.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_i2c
                       PRIV_REQUIRES log sa_config)
