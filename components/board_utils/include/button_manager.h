#pragma once

#include "pin_define.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

// 按钮标识
typedef enum
{
    BUTTON_BOOT = 0,
    BUTTON_1,
    BUTTON_2
} button_id_t;

/**
 * @brief 初始化按钮系统
 *
 * @return esp_err_t 成功返回ESP_OK
 */
esp_err_t button_init(void);

/**
 * @brief 获取按钮状态
 *
 * @param button_id 按钮ID
 * @param click 点击状态（输出参数）
 * @param hold 长按状态（输出参数）
 */
void button_get_state(button_id_t button_id, bool *click, bool *hold);

/**
 * @brief 清除按钮状态
 *
 * @param button_id 按钮ID
 */
void button_clear_state(button_id_t button_id);
