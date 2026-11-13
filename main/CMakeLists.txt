idf_component_register(SRCS "app_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES rules nvs_flash esp_app_format app_update)
