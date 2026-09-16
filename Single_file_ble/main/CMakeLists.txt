idf_component_register(SRCS "Single_file_ble.c"
                    INCLUDE_DIRS "."
                    REQUIRES bt nvs_flash esp_driver_spi esp_driver_gpio)
