idf_component_register(
    SRCS "servo.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio  esp_driver_ledc
)