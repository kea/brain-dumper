idf_component_register(
    SRCS "board.c" "buttons.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_adc esp_driver_usb_serial_jtag esp_hw_support esp_timer hal
)
