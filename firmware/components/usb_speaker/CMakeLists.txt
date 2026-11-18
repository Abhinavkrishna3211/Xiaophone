# USB speaker: the Xiaophone shows up as a USB sound card (phone or computer plays into it).
idf_component_register(
    SRCS "usb_speaker.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2s esp_timer usb_device_uac espressif__tinyusb lwip log heap
)
