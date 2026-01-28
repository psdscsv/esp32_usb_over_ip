#include "button_manager.h"
#include "freertos/queue.h"

static const char *TAG = "BUTTON";

// 按钮配置结构
typedef struct
{
    gpio_num_t gpio;
    button_id_t id;
    bool last_state;
    uint32_t press_time;
    bool click_event;
    bool hold_event;
} button_t;

// 按钮实例
static button_t buttons[] = {
    {BOOT_BUTTON_GPIO, BUTTON_BOOT, true, 0, false, false},
#ifdef BUTTON1_GPIO
    {BUTTON1_GPIO, BUTTON_1, true, 0, false, false},
#endif
    {BUTTON2_GPIO, BUTTON_2, true, 0, false, false}};

#define BUTTON_COUNT (sizeof(buttons) / sizeof(buttons[0]))

// 内部事件队列
static QueueHandle_t button_event_queue = NULL;

// 长按时间阈值（毫秒）
#define HOLD_THRESHOLD_MS 2000
#define BOOT_HOLD_THRESHOLD_MS 5000

// 防抖时间（毫秒）
#define DEBOUNCE_TIME_MS 50

// 按钮中断处理函数
static void IRAM_ATTR button_isr_handler(void *arg)
{
    button_id_t button_id = (button_id_t)(int)arg;

    // 发送事件到队列
    if (button_event_queue != NULL)
    {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;

        // 发送按钮ID到队列
        xQueueSendFromISR(button_event_queue, &button_id, &xHigherPriorityTaskWoken);

        if (xHigherPriorityTaskWoken)
        {
            portYIELD_FROM_ISR();
        }
    }
}

// 按钮处理任务
static void button_task(void *arg)
{
    button_id_t button_id;

    while (1)
    {
        // 等待按钮事件
        if (xQueueReceive(button_event_queue, &button_id, portMAX_DELAY) == pdTRUE)
        {
            button_t *btn = NULL;

            // 查找对应的按钮
            for (int i = 0; i < BUTTON_COUNT; i++)
            {
                if (buttons[i].id == button_id)
                {
                    btn = &buttons[i];
                    break;
                }
            }

            if (btn == NULL)
            {
                continue;
            }

            // 读取当前按钮状态
            bool current_state = gpio_get_level(btn->gpio);

            // 防抖处理
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_TIME_MS));
            bool stable_state = gpio_get_level(btn->gpio);

            if (current_state != stable_state)
            {
                // 状态不稳定，忽略
                continue;
            }

            if (current_state == 0)
            { // 按钮按下（低电平）
                btn->press_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
                btn->last_state = false;
            }
            else
            { // 按钮释放（高电平）
                if (!btn->last_state)
                { // 确保之前是按下的状态
                    uint32_t hold_time = (xTaskGetTickCount() * portTICK_PERIOD_MS) - btn->press_time;

                    // 判断是点击还是长按
                    uint32_t hold_threshold = (button_id == BUTTON_BOOT) ? BOOT_HOLD_THRESHOLD_MS : HOLD_THRESHOLD_MS;

                    if (hold_time >= hold_threshold)
                    {
                        btn->hold_event = true;
                        ESP_LOGI(TAG, "按钮 %d 长按: %d ms", button_id, hold_time);
                    }
                    else if (hold_time > DEBOUNCE_TIME_MS)
                    {
                        btn->click_event = true;
                        ESP_LOGI(TAG, "按钮 %d 点击", button_id);
                    }
                }
                btn->last_state = true;
            }
        }
    }
}

// 初始化按钮系统
esp_err_t button_init(void)
{
    esp_err_t ret = ESP_OK;

    // 创建事件队列
    button_event_queue = xQueueCreate(10, sizeof(button_id_t));
    if (button_event_queue == NULL)
    {
        ESP_LOGE(TAG, "创建按钮事件队列失败");
        return ESP_FAIL;
    }

    // 先安装GPIO中断服务（必须在添加中断处理器之前调用）
    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "安装GPIO中断服务失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 配置GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = 0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, // 启用上拉电阻
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE // 双边沿触发
    };

    // 为每个按钮配置GPIO
    for (int i = 0; i < BUTTON_COUNT; i++)
    {
        // 配置GPIO引脚
        io_conf.pin_bit_mask = (1ULL << buttons[i].gpio);
        ret = gpio_config(&io_conf);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "配置按钮 GPIO %d 失败: %s", buttons[i].gpio, esp_err_to_name(ret));
            return ret;
        }

        // 添加中断处理器（现在中断服务已经安装）
        ret = gpio_isr_handler_add(buttons[i].gpio, button_isr_handler, (void *)(int)buttons[i].id);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "安装按钮 %d 中断失败: %s", buttons[i].gpio, esp_err_to_name(ret));
            return ret;
        }

        // 使能GPIO中断
        gpio_intr_enable(buttons[i].gpio);
    }

    // 创建按钮处理任务
    xTaskCreate(button_task, "button_task", 2048, NULL, 5, NULL);

    ESP_LOGI(TAG, "按钮系统初始化完成，共 %d 个按钮", BUTTON_COUNT);
    return ESP_OK;
}

// 获取按钮状态
void button_get_state(button_id_t button_id, bool *click, bool *hold)
{
    if (click)
        *click = false;
    if (hold)
        *hold = false;

    for (int i = 0; i < BUTTON_COUNT; i++)
    {
        if (buttons[i].id == button_id)
        {
            if (click)
                *click = buttons[i].click_event;
            if (hold)
                *hold = buttons[i].hold_event;
            break;
        }
    }
}

// 清除按钮状态
void button_clear_state(button_id_t button_id)
{
    for (int i = 0; i < BUTTON_COUNT; i++)
    {
        if (buttons[i].id == button_id)
        {
            buttons[i].click_event = false;
            buttons[i].hold_event = false;
            break;
        }
    }
}