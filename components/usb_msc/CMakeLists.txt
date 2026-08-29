idf_component_register(
    SRCS "usb_msc.c"
    INCLUDE_DIRS "include"
    REQUIRES board storage hal espressif__esp_tinyusb
)
