#include "usbip_server.h"

#include <iostream>
#include <asio.hpp>
#include <spdlog/spdlog.h>
#include <nvs_flash.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <usb/usb_host.h>
#include <lwip/sys.h>
#include <lwip/sockets.h>

#include "Esp32Server.h"
#include "wifi_manager.h"

#include <lwip/tcp.h>
#include <lwip/netif.h>

using namespace std;

const char *UsbipServer::TAG = "usbip_server";

UsbipServer::UsbipServer()
{
    ESP_LOGI(TAG, "UsbipServer constructor");
}

UsbipServer::~UsbipServer()
{
    stop();
    ESP_LOGI(TAG, "UsbipServer destructor");
}

esp_pthread_cfg_t UsbipServer::create_config(const char *name, int core_id, int stack, int prio)
{
    auto cfg = esp_pthread_get_default_config();
    cfg.thread_name = name;
    cfg.pin_to_core = core_id;
    cfg.stack_size = stack;
    cfg.prio = prio;
    return cfg;
}

// 简化的IP事件处理器
void UsbipServer::ip_event_handler(void *arg, esp_event_base_t event_base,
                                   int32_t event_id, void *event_data)
{
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        auto *event = static_cast<ip_event_got_ip_t *>(event_data);
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
    }
}

// USB 事件任务函数
void UsbipServer::usb_host_event_task_func(void *arg)
{
    auto *server = static_cast<UsbipServer *>(arg);
    ESP_LOGI(server->TAG, "USB host event thread started on core %d", xPortGetCoreID());

    bool has_clients = true;
    bool has_devices = false;
    while (has_clients)
    {
        uint32_t event_flags;
        esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (err != ESP_OK)
        {
            ESP_LOGE(server->TAG, "USB host lib handle events error: %s", esp_err_to_name(err));
            break;
        }

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
        {
            ESP_LOGI(server->TAG, "No USB clients");
            if (ESP_OK == usb_host_device_free_all())
            {
                ESP_LOGI(server->TAG, "All devices marked as free");
                has_clients = false;
            }
            else
            {
                ESP_LOGI(server->TAG, "Waiting for all devices to be freed");
                has_devices = true;
            }
        }

        if (has_devices && (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE))
        {
            ESP_LOGI(server->TAG, "All USB devices freed");
            has_clients = false;
        }
    }

    ESP_LOGI(server->TAG, "Uninstalling USB Host Library");
    usb_host_uninstall();
    ESP_LOGI(server->TAG, "USB host event thread finished");
    vTaskDelete(NULL);
}

// 主工作线程任务函数
void UsbipServer::main_worker_task_func(void *arg)
{
    auto *server = static_cast<UsbipServer *>(arg);
    ESP_LOGI(server->TAG, "Main thread started on core %d", xPortGetCoreID());
    ESP_LOGI(server->TAG, "Thread start heap: %d bytes", esp_get_free_heap_size());

    try
    {
        server->thread_main();
    }
    catch (const std::exception &e)
    {
        ESP_LOGE(server->TAG, "Main thread exception: %s", e.what());
    }

    ESP_LOGI(server->TAG, "Thread end heap: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(server->TAG, "Main thread finished");
    vTaskDelete(NULL);
}

void UsbipServer::init_usb_host()
{
    ESP_LOGI(TAG, "Installing USB Host Library");

    // 使用零初始化然后设置字段的方式，避免字段顺序和缺失字段的问题
    usb_host_config_t host_config = {};
    host_config.skip_phy_setup = false;
    host_config.intr_flags = ESP_INTR_FLAG_LEVEL3;
    host_config.enum_filter_cb = nullptr;

    esp_err_t ret = usb_host_install(&host_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to install USB host: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "USB Host Library installed successfully");

    // 启动USB主机事件处理任务（使用 FreeRTOS API）
    xTaskCreatePinnedToCore(
        usb_host_event_task_func,
        "usb_host_event",
        4096,
        this,
        10,
        &usb_host_event_task,
        1);
}

void UsbipServer::init_server()
{
    ESP_LOGI(TAG, "Initializing USB host...");
    init_usb_host();

    ESP_LOGI(TAG, "All systems initialized");
}

void UsbipServer::thread_main()
{
    ESP_LOGI(TAG, "Starting main thread...");

    // 初始化所有系统
    init_server();

    // 设置spdlog日志级别
    spdlog::set_level(spdlog::level::trace);

    // 创建服务器实例
    server = std::make_unique<usbipdcpp::Esp32Server>();
    server->init_client();

    // 设置监听端点
    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), listening_port};

    // 启动服务器
    ESP_LOGI(TAG, "Starting USB/IP server on port %d", listening_port);
    server->start(endpoint);

    // 主循环
    ESP_LOGI(TAG, "Entering main loop...");
    while (true)
    {
        // 定期打印状态信息
        static int loop_count = 0;
        if (loop_count++ % 30 == 0)
        {
            wifi_status_t status;
            wifi_get_status(&status);

            if (status.connected)
            {
                ESP_LOGI(TAG, "System status: WiFi connected, IP: " IPSTR, IP2STR(&status.ip));
            }
            else
            {
                ESP_LOGI(TAG, "System status: WiFi disconnected");
            }

            ESP_LOGI(TAG, "Free heap: %d bytes", esp_get_free_heap_size());
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void UsbipServer::start()
{
    ESP_LOGI(TAG, "========== USB/IP Server Starting ==========");
    ESP_LOGI(TAG, "Application start");
    ESP_LOGI(TAG, "Free heap: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "Minimum free heap: %d bytes", esp_get_minimum_free_heap_size());

    // 创建主任务（使用 FreeRTOS API）
    xTaskCreatePinnedToCore(
        main_worker_task_func,
        "main_worker",
        8192,
        this,
        5,
        &main_worker_task,
        1 // 核心 1
    );
}

void UsbipServer::stop()
{
    ESP_LOGI(TAG, "Stopping USB/IP server...");

    // 停止服务器
    if (server)
    {
        server->stop();
        server.reset();
    }

    // 等待并删除主任务
    if (main_worker_task)
    {
        vTaskDelete(main_worker_task);
        main_worker_task = nullptr;
    }

    // 等待并删除 USB 事件任务
    if (usb_host_event_task)
    {
        vTaskDelete(usb_host_event_task);
        usb_host_event_task = nullptr;
    }

    ESP_LOGI(TAG, "========== USB/IP Server Finished ==========");
    ESP_LOGI(TAG, "Final free heap: %d bytes", esp_get_free_heap_size());
}