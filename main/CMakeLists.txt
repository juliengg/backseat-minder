idf_component_register(
    SRCS "usb_camera_stream.cpp" "usb_telemetry.cpp" "app_main.cpp" "setup_mode.cpp" "temp_humidity_sensor.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        modules
        esp_driver_gpio
        esp_driver_usb_serial_jtag
        esp_event
        esp_http_server
        esp_netif
        esp_rom
        esp_system
        esp_timer
        esp_wifi
        freertos
        log
        lwip
        nvs_flash
        espressif__esp32-camera
)
