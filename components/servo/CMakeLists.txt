idf_component_register(
    SRCS "servo.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver freertos esp_log
)
