#pragma once

#include <thread>
#include <memory>

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

private:
    static constexpr uint16_t listening_port = 3240;
    static const char *TAG;

    std::thread usb_host_event_thread;
    std::thread main_worker_thread;
    std::unique_ptr<usbipdcpp::Esp32Server> server;

    // 内部方法
    esp_pthread_cfg_t create_config(const char *name, int core_id, int stack, int prio);
    static void ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data);
    void init_usb_host();
    void init_server();
    void thread_main();
};