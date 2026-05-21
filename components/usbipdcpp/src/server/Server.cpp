#include "Server.h"

#include <thread>
#include <iostream>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/redirect_error.hpp>
#include <esp_log.h>

#include "utils.h"
#include "protocol.h"
#include "type.h"
#include "Session.h"
static const char *TAG = "usbipdcpp_Server";
usbipdcpp::Server::Server(std::vector<UsbDevice> &&devices)
{
    available_devices.reserve(devices.size());
    for (auto &device : devices)
    {
        available_devices.emplace_back(std::make_shared<UsbDevice>(std::move(device)));
    }
}

void usbipdcpp::Server::start(asio::ip::tcp::endpoint &ep)
{
    network_io_thread = std::thread([&, this]()
                                    {
        try {
            asio::ip::tcp::acceptor acceptor(asio_io_context);
            acceptor.open(ep.protocol());
            acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));

            acceptor.bind(ep);
            acceptor.listen();
            ESP_LOGI(TAG, "Listening on %s:%d", ep.address().to_string().c_str(), ep.port());
            asio::co_spawn(
                    asio_io_context,
                    do_accept(acceptor),
                    if_has_value_than_rethrow);
            asio_io_context.run();
        } catch (const std::exception &e) {
            ESP_LOGE(TAG, "An unexpected exception occurs in network thread: %s", e.what());
            std::exit(1);
        } });
}

void usbipdcpp::Server::stop()
{
    {
        std::shared_lock lock(session_list_mutex);
        for (auto &session : sessions)
        {
            if (auto shared_session = session.lock())
            {
                shared_session->immediately_stop();
            }
        }
    }
    // 虽然采取等待的策略好丑，但是代码好写啊
    while (true)
    {
        size_t alive_session_count = 0;
        {
            std::shared_lock lock(session_list_mutex);
            if (sessions.empty())
            {
                break;
            }
            alive_session_count = sessions.size();
        }
        auto wait_duration = std::chrono::seconds(1);
        ESP_LOGI(TAG, "There are still %d sessions that have not been closed, wait for %d seconds.",
                 alive_session_count, wait_duration.count());
        std::this_thread::sleep_for(wait_duration);
    }
    ESP_LOGI(TAG, "All sessions were successfully closed");

    // ESP_LOGI(TAG, "Successfully shut down transmissions for all devices");

    asio_io_context.stop();
    ESP_LOGD(TAG, "Successfully stop io_context");
    should_stop = true;
    network_io_thread.join();
}

void usbipdcpp::Server::add_device(std::shared_ptr<UsbDevice> &&device)
{
    std::lock_guard lock(devices_mutex);
    available_devices.emplace_back(device);
}

bool usbipdcpp::Server::has_bound_device(const std::string &busid)
{
    std::shared_lock lock(devices_mutex);
    // 只要存了这个设备就是有设备，不管是在可用设备还是正在使用的设备
    for (auto &device : available_devices)
    {
        if (device->busid == busid)
        {
            return true;
        }
    }
    return using_devices.contains(busid);
}

size_t usbipdcpp::Server::get_session_count()
{
    std::shared_lock lock(session_list_mutex);
    return sessions.size();
}

void usbipdcpp::Server::print_bound_devices()
{
    std::shared_lock lock(devices_mutex);

    std::size_t device_index = 1;
    ESP_LOGD(TAG, "available devices:");
    for (auto &device : available_devices)
    {
        ESP_LOGD(TAG, "\tNo.%zu device %s", device_index, device->busid.c_str());
        ++device_index;
    }
    device_index = 1;
    ESP_LOGD(TAG, "using devices:");
    for (auto &device : using_devices)
    {
        ESP_LOGD(TAG, "\tNo.%zu device %s", device_index, device.first.c_str());
        ++device_index;
    }
    ESP_LOGD(TAG, "");
}

void usbipdcpp::Server::register_session_exit_callback(std::function<void()> &&callback)
{
    std::lock_guard lock(session_list_mutex);
    session_exit_callbacks.emplace_back(std::move(callback));
}

void usbipdcpp::Server::on_session_exit()
{
    std::lock_guard lock(session_list_mutex);
    for (auto &callback : session_exit_callbacks)
    {
        callback();
    }
}

asio::awaitable<void> usbipdcpp::Server::do_accept(asio::ip::tcp::acceptor &acceptor)
{
    while (true)
    {
        if (should_stop)
        {
            ESP_LOGI(TAG, "Server stopping, exit accept loop");
            co_return;
        }

        ESP_LOGI(TAG, "Waiting for a new connection...");

        // 先创建一个Session，同时内部创建一个自己的socket
        auto session = std::make_shared<Session>(*this);

        asio::error_code ec;
        // 服务器io_context接收到socket后将其转移到session内部专有的io_context
        co_await acceptor.async_accept(session->socket, asio::redirect_error(asio::use_awaitable, ec));

        if (!ec)
        {
            // 设置TCP_NODELAY选项，减少延迟
            {
                asio::error_code set_ec;

                session->socket.set_option(asio::ip::tcp::no_delay(true), set_ec);
                if (set_ec)
                {
                    ESP_LOGW(TAG, "Failed to set TCP_NODELAY: %s", set_ec.message().c_str());
                }
                else
                {
                    ESP_LOGI(TAG, "TCP_NODELAY set successfully");
                }
            }

            {
                std::lock_guard lock(session_list_mutex);
                sessions.emplace_back(session);
            }

            auto remote_endpoint = session->socket.remote_endpoint();
            auto remote_endpoint_name = std::format("{}:{}",
                                                    remote_endpoint.address().to_string(),
                                                    remote_endpoint.port());
            ESP_LOGI(TAG, "A new connection from %s", remote_endpoint_name.c_str());

            // 函数会直接返回，但内部获取了自身的shared_ptr因此不会被析构
            // 每个session启动一个线程，防止某些必须阻塞的操作影响其他设备
            session->run();
        }
        else if (ec == asio::error::operation_aborted)
        {
            ESP_LOGI(TAG, "Operation aborted：%s", ec.message().c_str());
            break;
        }
        else
        {
            ESP_LOGE(TAG, "Connection error：%s", ec.message().c_str());
        }
    }
}

bool usbipdcpp::Server::is_device_using(const std::string &busid)
{
    std::shared_lock lock(devices_mutex);
    return using_devices.contains(busid);
}

void usbipdcpp::Server::try_moving_device_to_available(const std::string &busid)
{
    print_devices();
    ESP_LOGI(TAG, "尝试将%s转移到可用设备中", busid.c_str());
    std::lock_guard lock(devices_mutex);
    // ESP_LOGD(TAG, "成功获得两个锁");

    auto ret = using_devices.find(busid);
    if (ret != using_devices.end())
    {
        ESP_LOGI(TAG, "成功将%s转移到可用设备中", busid.c_str());
        auto &dev = ret->second;
        available_devices.emplace_back(std::move(dev));
        using_devices.erase(busid);
    }
    else
    {
        ESP_LOGW(TAG, "找不到busid为%s的设备", busid.c_str());
    }
}

std::shared_ptr<usbipdcpp::UsbDevice> usbipdcpp::Server::try_moving_device_to_using(const std::string &wanted_busid)
{
    std::lock_guard lock(devices_mutex);
    // 找能用的设备
    for (auto i = available_devices.begin(); i != available_devices.end(); ++i)
    {
        // 找到设备
        if (wanted_busid == (*i)->busid)
        {
            ESP_LOGI(TAG, "将%s放入正在使用的设备中", wanted_busid.c_str());
            // 将想要的设备放入正在使用的设备
            auto ret = (using_devices[wanted_busid] = std::move(*i));
            // 删掉可用设备中的这个设备
            available_devices.erase(i);
            return ret;
        }
    }
    ESP_LOGW(TAG, "找不到busid为%s的设备", wanted_busid.c_str());
    return nullptr;
}

void usbipdcpp::Server::print_devices()
{
    std::shared_lock guard(devices_mutex);
    ESP_LOGD(TAG, "有%d个可用设备", static_cast<int>(available_devices.size()));
    ESP_LOGD(TAG, "有%d个正在使用的设备，分别为", static_cast<int>(using_devices.size()));
    for (auto &dev : using_devices)
    {
        ESP_LOGD(TAG, "%s", dev.first.c_str());
    }
}
