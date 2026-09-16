idf_component_register(
    SRCS "sample-project.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_driver_spi
        esp_driver_gpio
)