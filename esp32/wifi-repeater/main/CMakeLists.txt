# esp_timer — для отсчёта времени, esp_driver_gpio — для светодиода статуса.
idf_component_register(SRCS "main.c"
                    PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_timer esp_driver_gpio
                    INCLUDE_DIRS ".")
