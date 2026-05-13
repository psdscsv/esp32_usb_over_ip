#pragma once

#include <memory>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// 包含必要的ESP-IDF头文件
extern "C"
{
#include <esp_event.h>
#include <esp_pthread.h>
}

// 前向声明
namespace usbipdcpp
{
    class Esp32Server;
}

class UsbipServer
{
public:
    UsbipServer();
    ~UsbipServer();

    // 启动服务器
    void start();

    // 停止服务器
    void stop();

    static const char *TAG;

private:
    static constexpr uint16_t listening_port = 3240;

    TaskHandle_t usb_host_event_task = nullptr;
    TaskHandle_t main_worker_task = nullptr;
    std::unique_ptr<usbipdcpp::Esp32Server> server;

    // 内部方法
    esp_pthread_cfg_t create_config(const char *name, int core_id, int stack, int prio);
    static void ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data);
    void init_usb_host();
    void init_server();
    void thread_main();

    // 静态任务函数
    static void usb_host_event_task_func(void *arg);
    static void main_worker_task_func(void *arg);
};