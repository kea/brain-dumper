idf_component_register(
    SRCS "web.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_event esp_netif esp_wifi esp_app_format esp_timer
             storage board net audio periph
    EMBED_FILES "www/index.html"
)
