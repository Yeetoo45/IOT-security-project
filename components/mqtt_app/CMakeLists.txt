idf_component_register(SRCS "mqtt_app.c"
                    INCLUDE_DIRS "include"
                    REQUIRES mqtt adxl345 json driver nvs_flash pinpad audio_player)