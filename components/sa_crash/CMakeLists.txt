idf_component_register(SRCS "sa_crash.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES espcoredump spi_flash log nvs_flash)
