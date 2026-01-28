idf_component_register(
    SRCS 
        "src/button_manager.c" 
        "src/led_manager.c" 
        "src/wifi_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES
        led_strip      # LED灯带组件
        esp_wifi       # wifi组件依赖
        nvs_flash
        esp_netif
        lwip
        esp_event      # 配网组件
        esp_http_server
        mdns
        driver
)