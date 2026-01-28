idf_component_register(
    SRCS "usbipd_server_test.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        asio
        spdlog
        nvs_flash
        esp_wifi
        pthread
        usb
        usbipdcpp
        board_utils
)

add_compile_definitions(
    # SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE
    SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO
    NDEBUG
)