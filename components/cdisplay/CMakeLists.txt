idf_component_register(
    SRCS "cdisplay.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer driver esp_driver_gpio esp_driver_i2c freertos shared
)
