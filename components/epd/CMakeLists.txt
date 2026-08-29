idf_component_register(
    SRCS "epd.c"
    INCLUDE_DIRS "include"
    REQUIRES driver board
    PRIV_REQUIRES esp_mm
)
