idf_component_register(
    SRCS "net.c" "stt.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event esp_http_client json nvs_flash storage periph
)
