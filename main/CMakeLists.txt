idf_component_register(
    SRCS "main.c" "app.c" "ui.c"
    INCLUDE_DIRS "."
    REQUIRES board epd ui_port periph storage audio net web usb_msc lvgl esp_timer
)
