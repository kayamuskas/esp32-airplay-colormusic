idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES raop alac esp_wifi nvs_flash mdns esp_event fastled esp_timer
)
