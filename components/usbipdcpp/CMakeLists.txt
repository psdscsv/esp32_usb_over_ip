# components/CMakeLists.txt

# 收集所有子目录中的源文件（支持 .c 和 .cpp）
file(GLOB_RECURSE COMPONENT_SRCS 
    "src/DeviceHandler/*.c"
    "src/DeviceHandler/*.cpp"
    "src/esp32_handler/*.c"
    "src/esp32_handler/*.cpp"
    "src/InterfaceHandler/*.c"
    "src/InterfaceHandler/*.cpp"
    "src/server/*.c"
    "src/server/*.cpp"
)

# 设置包含目录
set(COMPONENT_ADD_INCLUDEDIRS 
    "include/DeviceHandler"
    "include/esp32_handler"
    "include/InterfaceHandler"
    "include/server"
)

idf_component_register(SRCS ${COMPONENT_SRCS}
    INCLUDE_DIRS ${COMPONENT_ADD_INCLUDEDIRS}
    REQUIRES
        sock_utils
        asio
        spdlog
        usb
        esp_timer
        pthread
)