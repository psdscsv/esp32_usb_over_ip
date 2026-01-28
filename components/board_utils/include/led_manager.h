#pragma once

#include "pin_define.h"
#include <stdint.h>
#include <math.h>
#include "led_strip.h"

#ifdef __cplusplus
extern "C"
{
#endif

    // LED配置结构体
    typedef struct
    {
        uint8_t gpio_num;   // GPIO引脚号
        uint8_t led_num;    // LED数量
        uint8_t brightness; // 亮度 (0-255)
    } led_config_t;

    // LED句柄类型
    typedef struct
    {
        led_strip_handle_t strip_handle;
        led_config_t config;
    } led_handle_t;

    /**
     * @brief 初始化LED
     * @param config LED配置参数
     * @return LED句柄指针，失败返回NULL
     */
    led_handle_t *led_init(const led_config_t *config);

    /**
     * @brief 设置单个LED颜色
     * @param handle LED句柄
     * @param index LED索引
     * @param red 红色值 (0-255)
     * @param green 绿色值 (0-255)
     * @param blue 蓝色值 (0-255)
     * @return ESP_OK成功，其他失败
     */
    esp_err_t led_set_color(led_handle_t *handle, uint8_t index, uint8_t red, uint8_t green, uint8_t blue);

    /**
     * @brief 设置所有LED颜色
     * @param handle LED句柄
     * @param red 红色值 (0-255)
     * @param green 绿色值 (0-255)
     * @param blue 蓝色值 (0-255)
     * @return ESP_OK成功，其他失败
     */
    esp_err_t led_set_all(led_handle_t *handle, uint8_t red, uint8_t green, uint8_t blue);

    /**
     * @brief 清空所有LED
     * @param handle LED句柄
     * @return ESP_OK成功，其他失败
     */
    esp_err_t led_clear(led_handle_t *handle);

    /**
     * @brief 呼吸灯效果
     * @param handle LED句柄
     * @param red 红色值 (0-255)
     * @param green 绿色值 (0-255)
     * @param blue 蓝色值 (0-255)
     * @param duration_ms 持续时间(毫秒)
     */
    void led_breathing(led_handle_t *handle, uint8_t red, uint8_t green, uint8_t blue, uint32_t duration_ms);

    /**
     * @brief 彩虹灯效果
     * @param handle LED句柄
     * @param duration_ms 持续时间(毫秒)
     */
    void led_rainbow(led_handle_t *handle, uint32_t duration_ms);

    /**
     * @brief 闪烁效果
     * @param handle LED句柄
     * @param red 红色值 (0-255)
     * @param green 绿色值 (0-255)
     * @param blue 蓝色值 (0-255)
     * @param on_ms 亮的时间(毫秒)
     * @param off_ms 灭的时间(毫秒)
     * @param count 闪烁次数
     */
    void led_blink(led_handle_t *handle, uint8_t red, uint8_t green, uint8_t blue,
                   uint32_t on_ms, uint32_t off_ms, uint8_t count);

    /**
     * @brief 销毁LED，释放资源
     * @param handle LED句柄
     */
    void led_deinit(led_handle_t *handle);

    /**
     * @brief 设置亮度
     * @param handle LED句柄
     * @param brightness 亮度 (0-255)
     */
    void led_set_brightness(led_handle_t *handle, uint8_t brightness);

#ifdef __cplusplus
}
#endif
