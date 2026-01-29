#include "usbip_server.h"
#include "freertos/FreeRTOS.h"
#include <iostream>

#include "button_manager.h"
#include "wifi_manager.h"
#include "led_manager.h"

#include "esp_netif.h"
#include "lwip/opt.h"

using namespace std;
const char *TAG = "main";
// LED配置
static const led_config_t led_config = {
    .gpio_num = LED_PIN, // 假设在pin_define.h中定义了LED_GPIO
    .led_num = 1,        // LED数量
    .brightness = 64     // 亮度 (0-255)
};
static led_handle_t *led_handle = NULL;

void optimize_tcp_settings()
{
    // 增大TCP窗口大小
    lwip_tcp_set_recv_wnd(65535);
    lwip_tcp_set_send_buf(65535);

    // 开启TCP快速重传和快速恢复
    lwip_tcp_set_fast_retransmit(1);
    lwip_tcp_set_fast_recovery(1);

    // 增大最大分段大小（MSS）
    // 标准以太网MTU是1500，TCP头部40字节，所以MSS=1460
    // 可以通过调整MTU增加MSS
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif)
    {
        // 设置MTU为1600（如果路由器支持Jumbo Frame）
        esp_netif_set_mtu(netif, 1600);
        ESP_LOGI("Network", "MTU set to 1600");
    }
}

// 按钮状态监控任务
static void button_monitor_task(void *arg)
{
    ESP_LOGW(TAG, "按钮监控任务运行于核心 %d", xPortGetCoreID());
    bool click = false;
    bool hold = false;

    while (1)
    {
        // 检查BOOT按钮
        button_get_state(BUTTON_BOOT, &click, &hold);
        if (click)
        {
            printf("BOOT按钮被点击!\n");
            button_clear_state(BUTTON_BOOT);
            // 执行点击操作
        }
        if (hold)
        {
            printf("BOOT按钮被长按!\n");
            button_clear_state(BUTTON_BOOT);
            // 执行长按操作
        }

        // 检查按钮1
        button_get_state(BUTTON_1, &click, &hold);
        if (click)
        {
            printf("按钮1被点击!\n");
            button_clear_state(BUTTON_1);
            // 执行点击操作
        }
        if (hold)
        {
            printf("按钮1被长按!\n");
            button_clear_state(BUTTON_1);
            // 执行长按操作
        }

        // 检查按钮2
        button_get_state(BUTTON_2, &click, &hold);
        if (click)
        {
            printf("按钮2被点击!\n");
            button_clear_state(BUTTON_2);
            // 执行点击操作
        }
        if (hold)
        {
            printf("按钮2被长按!\n");
            button_clear_state(BUTTON_2);
            // 执行长按操作
        }

        vTaskDelay(50 / portTICK_PERIOD_MS); // 50ms检查一次
    }
}

void setup()
{
    // 初始化LED
    {
        led_handle = led_init(&led_config);
        if (led_handle == NULL)
        {
            printf("LED初始化失败!\n");
            return;
        }

        printf("LED初始化成功!\n");

        // 设置初始颜色
        led_set_all(led_handle, 32, 0, 32);
    }

    // 初始化NVS
    {
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
        {
            ESP_LOGI(TAG, "Erasing NVS flash...");
            ESP_ERROR_CHECK(nvs_flash_erase());
            ret = nvs_flash_init();
        }
        ESP_ERROR_CHECK(ret);
        ESP_LOGI(TAG, "NVS initialized");
        led_set_all(led_handle, 32, 0, 0);
    }

    // 初始化WiFi
    {
        wifi_init();

        ESP_LOGI(TAG, "Waiting for WiFi connection...");
        int wait_count = 0;
        const int max_wait = 30;

        while (wait_count < max_wait)
        {
            if (wifi_is_connected())
            {
                wifi_status_t status;
                wifi_get_status(&status);
                ESP_LOGI(TAG, "WiFi connected! IP: " IPSTR, IP2STR(&status.ip));
                break;
            }

            std::this_thread::sleep_for(std::chrono::seconds(1));
            wait_count++;

            if (wait_count % 10 == 0)
            {
                ESP_LOGI(TAG, "Still waiting for WiFi... (%d/%d seconds)", wait_count, max_wait);
            }
            led_set_all(led_handle, 32, 32, 0);
        }

        if (!wifi_is_connected())
        {
            ESP_LOGW(TAG, "WiFi connection timeout after %d seconds, continuing anyway", max_wait);
        }
    }

    // 初始化按钮
    {
        esp_err_t ret = button_init();
        if (ret != ESP_OK)
        {
            printf("按钮初始化失败!\n");
            return;
        }

        printf("按钮初始化成功!\n");

        // 创建按钮监控任务
        xTaskCreate(button_monitor_task, "btn_monitor", 4096, NULL, 5, NULL);
        led_set_all(led_handle, 0, 0, 32);
    }
}

// C入口函数
extern "C" void app_main(void)
{
    setup();

    // 创建服务器实例
    UsbipServer server;

    // 启动服务器
    server.start();
    vTaskDelay(pdMS_TO_TICKS(1));
    // 注意：这里不会执行到下面，因为start()会阻塞
    // 实际的停止应该在服务器内部处理
}