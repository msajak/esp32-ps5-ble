if(CONFIG_BT_CLASSIC_ENABLED)
    set(KB_SRC "btckb.cpp")
else()
    set(KB_SRC "blekb.cpp")
endif()

idf_component_register(
    SRCS ${KB_SRC} "keyboard_layouts.cpp"
    INCLUDE_DIRS "include"
    REQUIRES bt nvs_flash esp_timer driver freertos shared
)
