#pragma once
// 使能引脚
/*
Default log verbosity
    gpio_reset_pin(ENABLE_PIN);
    gpio_set_direction(ENABLE_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(ENABLE_PIN, 1);
*/
#define ENABLE_PIN 1
// LED引脚定义
#define LED_PIN 2
// 摄像头引脚定义
#define CAM_PWDN_GPIO_NUM -1
#define CAM_RESET_GPIO_NUM -1
#define CAM_XCLK_GPIO_NUM 8
#define CAM_SIOD_GPIO_NUM 13
#define CAM_SIOC_GPIO_NUM 12

#define CAM_Y2_GPIO_NUM 7
#define CAM_Y3_GPIO_NUM 5
#define CAM_Y4_GPIO_NUM 4
#define CAM_Y5_GPIO_NUM 6
#define CAM_Y6_GPIO_NUM 15
#define CAM_Y7_GPIO_NUM 17
#define CAM_Y8_GPIO_NUM 18
#define CAM_Y9_GPIO_NUM 9

#define CAM_VSYNC_GPIO_NUM 11
#define CAM_HREF_GPIO_NUM 10
#define CAM_PCLK_GPIO_NUM 16
// 按钮引脚定义
#define BOOT_BUTTON_GPIO 0 // 模式切换按钮
#define BUTTON1_GPIO 14    // 子选项切换按钮
#define BUTTON2_GPIO 39    // 确认按钮
