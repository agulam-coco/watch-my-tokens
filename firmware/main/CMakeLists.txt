idf_component_register(SRCS "main.c" "touch_driver.c"
                       PRIV_REQUIRES driver esp_driver_usb_serial_jtag esp_driver_i2c
                       INCLUDE_DIRS "")