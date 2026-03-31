#include "usbip_server.h"
#include "freertos/FreeRTOS.h"
#include <iostream>

// #include "button_manager.h"
#include "wifi_manager.h"
// #include "led_manager.h"
#include "pin_define.h"

#include <driver/gpio.h>
using namespace std;
const char *TAG = "main";
// LED配置
/*
static const led_config_t led_config = {
    .gpio_num = LED_PIN, // 假设在pin_define.h中定义了LED_GPIO
    .led_num = 1,        // LED数量
    .brightness = 64     // 亮度 (0-255)
};
static led_handle_t *led_handle = NULL;

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
*/
void setup()
{
vTaskDelay(1000 / portTICK_PERIOD_MS);
    // 初始化LED
    {
        // 1. 配置GPIO为输出模式
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << LED_PIN),    // 选择要配置的引脚
            .mode = GPIO_MODE_OUTPUT,             // 设置为输出模式
            .intr_type = GPIO_INTR_DISABLE,       // 禁止中断
        };
        gpio_config(&io_conf);

        // 2. 输出高电平
        gpio_set_level((gpio_num_t)LED_PIN, 1);
        /*
        led_handle = led_init(&led_config);
        if (led_handle == NULL)
        {
            printf("LED初始化失败!\n");
            return;
        }

        printf("LED初始化成功!\n");

        // 设置初始颜色
        led_set_all(led_handle, 32, 0, 32);
        */
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
        // led_set_all(led_handle, 32, 0, 0);
    }

    // 初始化WiFi
    {
        wifi_init();
    }
    /*
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
    */
}
// C入口函数
extern "C" void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(2000));
    setup();

    // 创建服务器实例
    UsbipServer server;

    // 启动服务器
    server.start();
    vTaskDelay(pdMS_TO_TICKS(1));
    // 注意：这里不会执行到下面，因为start()会阻塞
    // 实际的停止应该在服务器内部处理
}