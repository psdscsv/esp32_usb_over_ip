#pragma once

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "lwip/inet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include <string.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

// 配置参数
#define CONFIG_AP_SSID "登录-192.168.4.1"
#define CONFIG_AP_PASS ""
#define WEB_PORT 80
#define MAX_RECONNECTED_TIMES 2
    // WiFi状态结构体
    typedef struct
    {
        bool initialized;
        bool connected;
        bool ap_mode_active;
        char ssid[33];
        int8_t rssi;
        ip4_addr_t ip;
        ip4_addr_t gw;
        ip4_addr_t netmask;
    } wifi_status_t;

    /**
     * @brief 初始化WiFi系统
     *
     * 这个函数会：
     * 1. 初始化NVS
     * 2. 检查是否有保存的WiFi配置
     * 3. 如果有，尝试连接并等待结果
     * 4. 如果连接失败，启动AP热点和Web服务器等待用户配置
     */
    void wifi_init(void);

    /**
     * @brief 断开WiFi连接
     */
    void wifi_disconnect(void);

    /**
     * @brief 检查WiFi是否已连接
     *
     * @return true 已连接
     * @return false 未连接
     */
    bool wifi_is_connected(void);

    /**
     * @brief 获取WiFi状态信息
     *
     * @param status 状态信息结构体指针
     */
    void wifi_get_status(wifi_status_t *status);

    /**
     * @brief 扫描WiFi网络
     */
    void wifi_scan(void);

    /**
     * @brief 获取扫描结果
     *
     * @return const char* HTML格式的扫描结果
     */
    const char *wifi_get_scan_results(void);

    /**
     * @brief 重新连接WiFi
     */
    void wifi_reconnect(void);

#ifdef __cplusplus
}
#endif
