idf_component_register(SRCS "main.c"
                    PRIV_REQUIRES esp_wifi esp_netif lwip nvs_flash esp_event
                    INCLUDE_DIRS ".")
