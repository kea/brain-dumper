idf_component_register(
    SRCS "storage.c" "notes.c"
    INCLUDE_DIRS "include"
    REQUIRES board fatfs sdmmc esp_driver_sdmmc
)
