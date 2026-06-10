idf_component_register(
    SRCS "blekb.cpp" "keyboard_layouts.cpp"
    INCLUDE_DIRS "include"
    REQUIRES bt nvs_flash esp_timer driver freertos shared
)
