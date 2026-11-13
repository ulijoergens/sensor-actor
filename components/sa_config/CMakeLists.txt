idf_component_register(SRCS "sa_config.c" "sa_config_validate.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES nvs_flash esp_hw_support log)
