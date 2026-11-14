idf_component_register(SRCS "webui.c" "webui_sensors.c" "webui_actors.c" "webui_crash.c"
                       INCLUDE_DIRS "include"
                       EMBED_TXTFILES "www/index.html" "www/app.js" "www/style.css"
                       PRIV_REQUIRES esp_http_server esp_app_format app_update esp_timer log
                                     sa_config sa_log sa_mqtt sensors actors sa_crash wifi_mgr)
