file(GLOB TEST_SRCS "${CMAKE_CURRENT_LIST_DIR}/../../../components/*/test/test_*.c")
idf_component_register(SRCS "test_main.c" ${TEST_SRCS}
                       REQUIRES unity rules sa_config sa_i2c sa_mqtt sensors wifi_mgr
                       WHOLE_ARCHIVE)
