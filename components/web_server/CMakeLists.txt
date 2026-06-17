idf_component_register(
    SRCS "web_server.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server wifi_mgmt blekb ota shared logger servo
    EMBED_FILES "web_ui/index.html"
)
