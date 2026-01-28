#include <iostream>
#include <thread>

#include <nvs_flash.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_pthread.h>
#include <usb/usb_host.h>

#include <lwip/sys.h>
#include <lwip/sockets.h>

#include <asio.hpp>
#include <spdlog/spdlog.h>

#include <pthread.h>
#include "esp32_handler/Esp32Server.h"
#include "mock_mouse.h"
#include "wifi_manager.h"

using namespace std;

auto TAG = "usbip_server";
constexpr std::uint16_t listening_port = 3240;

esp_pthread_cfg_t create_config(const char *name, int core_id, int stack, int prio)
{
    auto cfg = esp_pthread_get_default_config();
    cfg.thread_name = name;
    cfg.pin_to_core = core_id;
    cfg.stack_size = stack;
    cfg.prio = prio;
    return cfg;
}

// 简化的IP事件处理器
static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        auto *event = static_cast<ip_event_got_ip_t *>(event_data);
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
    }
}

std::thread usb_host_event_thread;

void init_usb_host()
{
    ESP_LOGI(TAG, "Installing USB Host Library");

    // 最简单的初始化方式 - 使用默认配置
    usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        .enum_filter_cb = nullptr,
        .enum_filter_arg = nullptr};

    esp_err_t ret = usb_host_install(&host_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to install USB host: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "USB Host Library installed successfully");

    // 配置USB主机事件线程
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.prio = 10;
    cfg.pin_to_core = 1; // 核心1
    cfg.thread_name = "usb_host_event_thread";
    cfg.stack_size = 4096;
    esp_pthread_set_cfg(&cfg);

    // 启动USB主机事件处理线程
    usb_host_event_thread = std::thread([]()
                                        {
        ESP_LOGI(TAG, "USB host event thread started");
        
        bool has_clients = true;
        bool has_devices = false;
        while (has_clients) {
            uint32_t event_flags;
            esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "USB host lib handle events error: %s", esp_err_to_name(err));
                break;
            }
            
            if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
                ESP_LOGI(TAG, "No USB clients");
                if (ESP_OK == usb_host_device_free_all()) {
                    ESP_LOGI(TAG, "All devices marked as free");
                    has_clients = false;
                }
                else {
                    ESP_LOGI(TAG, "Waiting for all devices to be freed");
                    has_devices = true;
                }
            }
            
            if (has_devices && (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE)) {
                ESP_LOGI(TAG, "All USB devices freed");
                has_clients = false;
            }
        }
        
        ESP_LOGI(TAG, "Uninstalling USB Host Library");
        usb_host_uninstall();
        
        ESP_LOGI(TAG, "USB host event thread finished"); });

    // 恢复默认线程配置
    esp_pthread_cfg_t default_cfg = esp_pthread_get_default_config();
    esp_pthread_set_cfg(&default_cfg);
}

// 注册IP事件处理器（可选）
void register_ip_event_handler()
{
    esp_event_handler_instance_t ip_event_instance;
    esp_err_t ret = esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &ip_event_handler,
                                                        NULL,
                                                        &ip_event_instance);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register IP event handler: %s", esp_err_to_name(ret));
    }
    else
    {
        ESP_LOGI(TAG, "IP event handler registered");
    }
}

void init_all()
{
    ESP_LOGI(TAG, "Initializing all systems...");

    // 1. 初始化NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGI(TAG, "Erasing NVS flash...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");

    // 2. 使用wifi_manager初始化WiFi
    ESP_LOGI(TAG, "Initializing WiFi using wifi_manager...");
    wifi_init();

    // 3. 注册IP事件处理器（可选）
    register_ip_event_handler();

    // 4. 等待WiFi连接（可选，如果wifi_manager是阻塞的则不需要）
    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    int wait_count = 0;
    const int max_wait = 30; // 最多等待30秒

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
    }

    if (!wifi_is_connected())
    {
        ESP_LOGW(TAG, "WiFi connection timeout after %d seconds, continuing anyway", max_wait);
    }

    // 5. 初始化USB主机
    ESP_LOGI(TAG, "Initializing USB host...");
    init_usb_host();

    ESP_LOGI(TAG, "All systems initialized");
}

asio::awaitable<void> handle_connection(asio::ip::tcp::socket &&socket)
{
    ESP_LOGI(TAG, "New TCP connection accepted");

    while (true)
    {
        try
        {
            std::array<char, 1> buffer{};
            co_await asio::async_read(socket, asio::buffer(buffer), asio::use_awaitable);
            co_await asio::async_write(socket, asio::buffer(buffer), asio::use_awaitable);
        }
        catch (std::exception &e)
        {
            ESP_LOGE(TAG, "Socket exception: %s", e.what());
            break;
        }
    }

    ESP_LOGI(TAG, "Closing socket");
    std::error_code ignore_ec;
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignore_ec);
    socket.close(ignore_ec);

    co_return;
}

using namespace usbipdcpp;

int thread_main()
{
    ESP_LOGI(TAG, "Starting main thread...");

    // 初始化所有系统
    init_all();

    // 设置spdlog日志级别
    spdlog::set_level(spdlog::level::trace);

    // 创建服务器实例
    Esp32Server server;
    server.init_client();

    // 设置监听端点
    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), listening_port};

    // 启动服务器
    ESP_LOGI(TAG, "Starting USB/IP server on port %d", listening_port);
    server.start(endpoint);

    // 主循环
    ESP_LOGI(TAG, "Entering main loop...");
    while (true)
    {
        // 定期打印状态信息
        static int loop_count = 0;
        if (loop_count++ % 30 == 0) // 每30秒打印一次
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

    // 清理（通常不会执行到这里）
    server.stop();
    ESP_LOGI(TAG, "Server stopped");

    return 0;
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "========== USB/IP Server Starting ==========");
    ESP_LOGI(TAG, "Application start");
    ESP_LOGI(TAG, "Free heap: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "Minimum free heap: %d bytes", esp_get_minimum_free_heap_size());

    // 创建主线程
    std::thread main_thread([&]()
                            {
        ESP_LOGI(TAG, "Main thread started");
        ESP_LOGI(TAG, "Thread start heap: %d bytes", esp_get_free_heap_size());
        
        try
        {
            thread_main();
        }
        catch (const std::exception &e)
        {
            ESP_LOGE(TAG, "Main thread exception: %s", e.what());
        }
        
        ESP_LOGI(TAG, "Thread end heap: %d bytes", esp_get_free_heap_size());
        ESP_LOGI(TAG, "Main thread finished"); });

    // 设置主线程配置
    esp_pthread_cfg_t main_cfg = create_config("main_thread", 0, 8192, 5);
    esp_pthread_set_cfg(&main_cfg);

    // 等待主线程结束
    main_thread.join();

    ESP_LOGI(TAG, "========== USB/IP Server Finished ==========");
    ESP_LOGI(TAG, "Final free heap: %d bytes", esp_get_free_heap_size());
}
