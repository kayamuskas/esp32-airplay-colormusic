
idf_component_register(
    SRCS
        dmap_parser.c
        raop.c
        rtp.c
        util.c
        log_util.c
    INCLUDE_DIRS .
    PRIV_REQUIRES freertos pthread mdns esp_netif lwip mbedtls esp_timer alac
)

set_source_files_properties(raop.c PROPERTIES COMPILE_FLAGS -Wno-misleading-indentation)
