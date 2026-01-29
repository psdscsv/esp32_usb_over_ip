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

#define CONFIG_AP_SSID "登录-192.168.4.1"
#define CONFIG_AP_PASS ""
#define WEB_PORT 80
#define MAX_RECONNECTED_TIMES 2

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

    void wifi_init(void);
    void wifi_disconnect(void);
    bool wifi_is_connected(void);
    void wifi_get_status(wifi_status_t *status);
    void wifi_scan(void);
    const char *wifi_get_scan_results(void);
    void wifi_reconnect(void);

#ifdef __cplusplus
}
#endif