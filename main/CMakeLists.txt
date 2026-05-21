idf_component_register(
    SRCS 
    "usbipd_server.cpp"
    "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        sock_utils 
        asio
        nvs_flash
        pthread
        usb
        usbipdcpp
        board_utils
        esp_timer
)

add_compile_definitions(
    NDEBUG
)