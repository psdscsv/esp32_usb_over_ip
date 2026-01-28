#include "led_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static const char *TAG = "LED";

// 初始化LED
led_handle_t *led_init(const led_config_t *config)
{
    // 分配内存
    led_handle_t *handle = calloc(1, sizeof(led_handle_t));

    if (!config)
    {
        ESP_LOGE(TAG, "Config is None");
        return NULL;
    }

    if (!handle)
    {
        ESP_LOGE(TAG, "Failed to allocate memory");
        return NULL;
    }

    // 保存配置
    handle->config = *config;

    // 配置LED strip
    led_strip_config_t strip_config = {
        .strip_gpio_num = config->gpio_num,
        .max_leds = config->led_num,
    };

    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        .flags.with_dma = false,
    };

    // 创建LED设备
    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &handle->strip_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create LED strip device");
        free(handle);
        return NULL;
    }

    // 清空LED
    led_clear(handle);

    ESP_LOGI(TAG, "LED initialized on GPIO %d, %d LEDs", config->gpio_num, config->led_num);
    return handle;
}

// 设置单个LED颜色
esp_err_t led_set_color(led_handle_t *handle, uint8_t index, uint8_t red, uint8_t green, uint8_t blue)
{
    if (!handle || !handle->strip_handle)
    {
        ESP_LOGE(TAG, "Invalid handle");
        return ESP_FAIL;
    }

    if (index >= handle->config.led_num)
    {
        ESP_LOGE(TAG, "LED index out of range");
        return ESP_FAIL;
    }

    // 应用亮度
    uint8_t brightness = handle->config.brightness;
    red = red * brightness / 255;
    green = green * brightness / 255;
    blue = blue * brightness / 255;

    led_strip_set_pixel(handle->strip_handle, index, red, green, blue);
    return led_strip_refresh(handle->strip_handle);
}

// 设置所有LED颜色
esp_err_t led_set_all(led_handle_t *handle, uint8_t red, uint8_t green, uint8_t blue)
{
    if (!handle || !handle->strip_handle)
    {
        ESP_LOGE(TAG, "Invalid handle");
        return ESP_FAIL;
    }

    // 应用亮度
    uint8_t brightness = handle->config.brightness;
    red = red * brightness / 255;
    green = green * brightness / 255;
    blue = blue * brightness / 255;

    for (uint8_t i = 0; i < handle->config.led_num; i++)
    {
        led_strip_set_pixel(handle->strip_handle, i, red, green, blue);
    }
    return led_strip_refresh(handle->strip_handle);
}

// 清空所有LED
esp_err_t led_clear(led_handle_t *handle)
{
    if (!handle || !handle->strip_handle)
    {
        ESP_LOGE(TAG, "Invalid handle");
        return ESP_FAIL;
    }

    return led_strip_clear(handle->strip_handle);
}

// 呼吸灯效果
void led_breathing(led_handle_t *handle, uint8_t red, uint8_t green, uint8_t blue, uint32_t duration_ms)
{
    if (!handle)
        return;

    uint32_t steps = duration_ms / 20; // 每20ms一步
    for (uint32_t i = 0; i < steps; i++)
    {
        float brightness = (sinf(2 * M_PI_2 * i / steps - M_PI_2) + 1.0f) / 2.0f;
        uint8_t r = red * brightness;
        uint8_t g = green * brightness;
        uint8_t b = blue * brightness;
        led_set_all(handle, r, g, b);
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
}

// 彩虹灯效果
void led_rainbow(led_handle_t *handle, uint32_t duration_ms)
{
    if (!handle)
        return;

    uint32_t steps = duration_ms / 20; // 每20ms一步
    for (uint32_t i = 0; i < steps; i++)
    {
        float hue = (float)i / steps; // 0.0到1.0
        float r, g, b;

        // HSV转RGB
        float h = hue * 6.0f;
        float s = 1.0f;
        float v = 1.0f;

        int hi = (int)h;
        float f = h - hi;
        float p = v * (1 - s);
        float q = v * (1 - f * s);
        float t = v * (1 - (1 - f) * s);

        switch (hi)
        {
        case 0:
            r = v;
            g = t;
            b = p;
            break;
        case 1:
            r = q;
            g = v;
            b = p;
            break;
        case 2:
            r = p;
            g = v;
            b = t;
            break;
        case 3:
            r = p;
            g = q;
            b = v;
            break;
        case 4:
            r = t;
            g = p;
            b = v;
            break;
        default:
            r = v;
            g = p;
            b = q;
            break;
        }

        led_set_all(handle, r * 255, g * 255, b * 255);
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
}

// 闪烁效果
void led_blink(led_handle_t *handle, uint8_t red, uint8_t green, uint8_t blue,
               uint32_t on_ms, uint32_t off_ms, uint8_t count)
{
    if (!handle)
        return;

    for (uint8_t i = 0; i < count; i++)
    {
        led_set_all(handle, red, green, blue);
        vTaskDelay(on_ms / portTICK_PERIOD_MS);
        led_clear(handle);
        vTaskDelay(off_ms / portTICK_PERIOD_MS);
    }
}

// 销毁LED
void led_deinit(led_handle_t *handle)
{
    if (handle)
    {
        if (handle->strip_handle)
        {
            led_strip_del(handle->strip_handle);
        }
        free(handle);
        ESP_LOGI(TAG, "LED deinitialized");
    }
}

// 设置亮度
void led_set_brightness(led_handle_t *handle, uint8_t brightness)
{
    if (handle)
    {
        handle->config.brightness = brightness;
    }
}