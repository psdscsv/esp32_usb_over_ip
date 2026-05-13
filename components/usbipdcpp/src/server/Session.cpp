#include "Session.h"

#include <asio.hpp>
#include <spdlog/spdlog.h>
#include <asio/experimental/parallel_group.hpp>
#include <asio/experimental/awaitable_operators.hpp>

#include "Server.h"
#include "device.h"
#include "protocol.h"
#include "utils.h"

#include <lwip/sockets.h>
#include "esp_log.h"

usbipdcpp::Session::Session(Server &server) : server(server),
                                              socket(session_io_context)
{
}

std::tuple<bool, std::uint32_t> usbipdcpp::Session::get_unlink_seqnum(std::uint32_t seqnum)
{
    std::shared_lock lock(unlink_map_mutex);
    if (unlink_map.contains(seqnum))
    {
        return {true, unlink_map[seqnum]};
    }
    return {false, 0};
}

void usbipdcpp::Session::remove_seqnum_unlink(std::uint32_t seqnum)
{
    std::lock_guard lock(unlink_map_mutex);
    unlink_map.erase(seqnum);
}

void usbipdcpp::Session::submit_ret_unlink_and_then_remove_seqnum_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink,
                                                                         std::uint32_t seqnum)
{
    submit_ret_unlink(std::move(unlink));
    remove_seqnum_unlink(seqnum);
}

void usbipdcpp::Session::submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink)
{
    SPDLOG_DEBUG("收到提交的unlink包 {}", unlink.header.seqnum);
    error_code send_ec;
    transfer_channel->async_send(send_ec, UsbIpResponse::RetVariant{std::move(unlink)}, asio::detached);
    if (send_ec)
    {
        SPDLOG_ERROR("transfer_channel async_send unlink seq={} error: {}", unlink.header.seqnum, send_ec.message());
    }
    else
    {
        SPDLOG_DEBUG("transfer_channel async_send unlink seq={} queued", unlink.header.seqnum);
    }
}

void usbipdcpp::Session::submit_ret_submit(UsbIpResponse::UsbIpRetSubmit &&submit)
{
    SPDLOG_DEBUG("收到提交的submit包{}", submit.header.seqnum);
    error_code send_ec;
    transfer_channel->async_send(send_ec, UsbIpResponse::RetVariant{std::move(submit)}, asio::detached);
    if (send_ec)
    {
        SPDLOG_ERROR("transfer_channel async_send submit seq={} error: {}", submit.header.seqnum, send_ec.message());
    }
    else
    {
        SPDLOG_TRACE("transfer_channel async_send submit seq={} queued", submit.header.seqnum);
    }
}

usbipdcpp::Session::~Session()
{
    SPDLOG_TRACE("Session析构");
}

void usbipdcpp::Session::run()
{
    auto self = shared_from_this();
    asio::co_spawn(session_io_context, [this]()
                   { return parse_op(); }, if_has_value_than_rethrow);

    SPDLOG_TRACE("创建Session线程");
    run_thread = std::thread([self = std::move(self)]()
                             {
        self->session_io_context.run();

        //处理结束后自动往服务器中删除自身
        {
            std::lock_guard lock(self->server.session_list_mutex);
            for (auto it = self->server.sessions.begin(); it != self->server.sessions.end();) {
                if (auto s = it->lock()) {
                    if (s == self) {
                        it = self->server.sessions.erase(it);
                        break;
                    }
                    else {
                        ++it;
                    }
                }
                else {
                    it = self->server.sessions.erase(it);
                }
            }
        }
        self->server.on_session_exit();
        self->run_thread.detach(); });
}

asio::awaitable<void> usbipdcpp::Session::parse_op()
{
    usbipdcpp::error_code ec;
    SPDLOG_TRACE("尝试读取OP");
    auto op = co_await UsbIpCommand::get_op_from_socket(socket, ec);
    if (ec)
    {
        SPDLOG_DEBUG("从socket中获取op时出错：{}", ec.message());
        if (ec.value() == static_cast<int>(ErrorType::SOCKET_EOF))
        {
            SPDLOG_DEBUG("连接关闭");
        }
        else if (ec.value() == static_cast<int>(ErrorType::SOCKET_ERR))
        {
            SPDLOG_DEBUG("发生socket错误");
        }

        goto close_socket;
    }
    co_await std::visit([&, this](auto &&cmd) -> asio::awaitable<void>
                        {
        using T = std::remove_cvref_t<decltype(cmd)>;
        if constexpr (std::is_same_v<UsbIpCommand::OpReqDevlist, T>) {
            SPDLOG_TRACE("收到 OpReqDevlist 包");
            data_type to_be_sent;
            {
                std::shared_lock lock(server.devices_mutex);
                to_be_sent = UsbIpResponse::OpRepDevlist::create_from_devices(server.available_devices).to_bytes();
            }
            co_await asio::async_write(socket, asio::buffer(to_be_sent), asio::use_awaitable);
            SPDLOG_TRACE("成功发送 OpRepDevlist 包");
        }
        else if constexpr (std::is_same_v<UsbIpCommand::OpReqImport, T>) {
            SPDLOG_TRACE("收到 OpReqImport 包");
            auto wanted_busid = std::string(reinterpret_cast<char *>(cmd.busid.data()));
            UsbIpResponse::OpRepImport op_rep_import{};
            SPDLOG_TRACE("客户端想连接busid为 {} 的设备", wanted_busid);

            bool target_device_is_using = false;
            if (server.is_device_using(wanted_busid)) {
                spdlog::warn("正在使用的设备不支持导出");
                op_rep_import = UsbIpResponse::OpRepImport::create_on_failure_with_status(
                        static_cast<std::uint32_t>(OperationStatuType::NA));
                target_device_is_using = true;
            }
            else {
                if (auto using_device = server.try_moving_device_to_using(wanted_busid)) {
                    std::lock_guard lock(current_import_device_data_mutex);
                    spdlog::info("成功将设备放入正在使用的设备中");
                    current_import_device_id = wanted_busid;
                    current_import_device = using_device;
                    spdlog::info("成功缓存正在使用的设备");
                }
            }

            std::shared_lock lock(current_import_device_data_mutex);
            if (!target_device_is_using) {
                if (current_import_device) {
                    spdlog::info("找到目标设备，可以导入");
                    op_rep_import = UsbIpResponse::OpRepImport::create_on_success(current_import_device);
                    cmd_transferring = true;
                }
                else {
                    spdlog::info("不存在目标设备，不可导入");
                    op_rep_import = UsbIpResponse::OpRepImport::create_on_failure_with_status(
                            static_cast<std::uint32_t>(OperationStatuType::NoDev));
                }
                auto to_be_sent = op_rep_import.to_bytes();
                co_await asio::async_write(socket, asio::buffer(to_be_sent), asio::use_awaitable);
                SPDLOG_TRACE("成功发送 OpRepImport 包");
            }

            if (cmd_transferring) {
                usbipdcpp::error_code transferring_ec;
                co_await transfer_loop(transferring_ec);
                if (transferring_ec) {
                    SPDLOG_ERROR("Error occurred during transferring : {}", transferring_ec.message());
                    ec = transferring_ec;
                }
            }
        }
        else if constexpr (std::is_same_v<std::monostate, T>) {
            SPDLOG_ERROR("收到未知包");
            ec = make_error_code(ErrorType::UNKNOWN_CMD);
        }
        else {
            static_assert(!std::is_same_v<T, T>);
        } }, op);

    if (this->transfer_channel)
    {
        this->transfer_channel->close();
    }

close_socket:
    std::error_code ignore_ec;
    SPDLOG_INFO("尝试关闭socket，原因: {}", ec.message());
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignore_ec);
    socket.close(ignore_ec);
}

void usbipdcpp::Session::immediately_stop()
{
    should_immediately_stop = true;

    SPDLOG_INFO("session immediately_stop called");

    asio::post(session_io_context,
               [this]()
               {
                   this->socket.close();
                   if (this->transfer_channel)
                   {
                       this->transfer_channel->close();
                   }
               });
}

asio::awaitable<void> usbipdcpp::Session::transfer_loop(usbipdcpp::error_code &transferring_ec)
{
    current_import_device->on_new_connection(*this, transferring_ec);
    if (transferring_ec)
        co_return;

    error_code receiver_ec;
    error_code sender_ec;
    using namespace asio::experimental::awaitable_operators;

    transfer_channel = std::make_unique<transfer_channel_type>(server.asio_io_context, transfer_channel_size);

    co_await (receiver(receiver_ec) && sender(sender_ec));

    if (sender_ec)
    {
        SPDLOG_ERROR("An error occur during sending: {}", sender_ec.message());
        transferring_ec = sender_ec;
    }
    else if (receiver_ec)
    {
        SPDLOG_ERROR("An error occur during receiving: {}", receiver_ec.message());
        transferring_ec = receiver_ec;
    }
    cmd_transferring = false;
}

asio::awaitable<void> usbipdcpp::Session::receiver(usbipdcpp::error_code &receiver_ec)
{
    co_await receiver_single(receiver_ec);
}

asio::awaitable<void> usbipdcpp::Session::receiver_single(usbipdcpp::error_code &receiver_ec)
{
    spdlog::info("should_immediately_stop:{}", should_immediately_stop.load());
    while (!should_immediately_stop)
    {
        usbipdcpp::error_code ec;

        auto command = co_await UsbIpCommand::get_cmd_from_socket(socket, ec);
        if (ec)
        {
            SPDLOG_DEBUG("从socket中获取命令时出错：{}", ec.message());
            if (ec.value() == static_cast<int>(ErrorType::SOCKET_EOF))
            {
                SPDLOG_DEBUG("连接关闭");
            }
            else if (ec.value() == static_cast<int>(ErrorType::SOCKET_ERR))
            {
                SPDLOG_DEBUG("发生socket错误");
            }
            break;
        }
        else
        {
            if (should_immediately_stop)
                break;
            co_await std::visit([&, this](auto &&cmd) -> asio::awaitable<void>
                                {
                using T = std::remove_cvref_t<decltype(cmd)>;
                if constexpr (std::is_same_v<UsbIpCommand::UsbIpCmdSubmit, T>) {
                    UsbIpCommand::UsbIpCmdSubmit &cmd2 = cmd;
                    SPDLOG_TRACE("收到 UsbIpCmdSubmit 包，序列号: {}", cmd2.header.seqnum);
                    auto out = cmd2.header.direction == UsbIpDirection::Out;
                    SPDLOG_TRACE("Usbip传输方向为：{}", out ? "out" : "in");
                    std::uint8_t real_ep = out
                                               ? static_cast<std::uint8_t>(cmd2.header.ep)
                                               : (static_cast<std::uint8_t>(cmd2.header.ep) | 0x80);
                    SPDLOG_TRACE("传输的真实端口为 {:02x}", real_ep);
                    auto current_seqnum = cmd2.header.seqnum;

                    auto ep_find_ret = current_import_device->find_ep(real_ep);

                    if (ep_find_ret.has_value()) {
                        auto &ep = ep_find_ret->first;
                        auto &intf = ep_find_ret->second;

                        SPDLOG_TRACE("->端口{0:02x}", ep.address);
                        SPDLOG_TRACE("->setup数据{}", get_every_byte(cmd2.setup.to_bytes()));
                        SPDLOG_TRACE("->请求数据{}", get_every_byte(cmd2.data));

                        usbipdcpp::error_code ec_during_handling_urb;
                        current_import_device->handle_urb(
                                cmd2,
                                current_seqnum,
                                ep,
                                intf,
                                cmd2.transfer_buffer_length, cmd2.setup, cmd2.data, cmd2.iso_packet_descriptor,
                                ec_during_handling_urb
                                );

                        if (ec_during_handling_urb) {
                            SPDLOG_ERROR("Error during handling urb : {}", ec_during_handling_urb.message());
                            receiver_ec = ec_during_handling_urb;
                            should_immediately_stop = true;
                            co_return;
                        }
                    }
                    else {
                        SPDLOG_WARN("找不到端点{}", real_ep);
                        UsbIpResponse::UsbIpRetSubmit ret_submit;
                        ret_submit = UsbIpResponse::UsbIpRetSubmit::usbip_ret_submit_fail_with_status(
                                cmd2.header.seqnum,EPIPE);
                        auto to_be_sent = ret_submit.to_bytes();
                        co_await asio::async_write(socket, asio::buffer(to_be_sent), asio::use_awaitable);
                        SPDLOG_TRACE("成功发送 UsbIpRetSubmit 包");
                    }
                }
                else if constexpr (std::is_same_v<UsbIpCommand::UsbIpCmdUnlink, T>) {
                    UsbIpCommand::UsbIpCmdUnlink &cmd2 = cmd;
                    SPDLOG_TRACE("收到 UsbIpCmdUnlink 包，序列号: {}", cmd2.header.seqnum);
                    int64_t recv_time = esp_timer_get_time();
                    {
                        std::unique_lock lock(timestamps_mutex_);
                        recv_timestamps_[cmd2.header.seqnum] = recv_time;
                    }
                    {
                        std::lock_guard lock(unlink_map_mutex);
                        unlink_map.emplace(cmd2.unlink_seqnum, cmd2.header.seqnum);
                    }
                    current_import_device->handle_unlink_seqnum(cmd2.unlink_seqnum);
                }
                else if constexpr (std::is_same_v<std::monostate, T>) {
                    SPDLOG_ERROR("收到未知包");
                    receiver_ec = make_error_code(ErrorType::UNKNOWN_CMD);
                }
                else {
                    static_assert(!std::is_same_v<T, T>);
                }
                co_return; }, command);
        }
    }
    current_import_device->on_disconnection(receiver_ec);
    transfer_channel->close();

    server.try_moving_device_to_available(*current_import_device_id);
    current_import_device_id.reset();
    current_import_device.reset();
    SPDLOG_TRACE("将当前导入设备的busid设为空");
}

asio::awaitable<void> usbipdcpp::Session::sender(usbipdcpp::error_code &ec)
{
    while (!should_immediately_stop)
    {
        auto send_data = co_await transfer_channel->async_receive(asio::redirect_error(asio::use_awaitable, ec));
        if (ec)
        {
            if (ec != asio::experimental::error::channel_closed)
            {
                SPDLOG_ERROR("transfer_channel async_receive error: {}", ec.message());
            }
            break;
        }

        SPDLOG_TRACE("channel收到消息，准备发送");
        error_code sending_ec;

        co_await std::visit([&](auto &&cmd) -> asio::awaitable<void>
                            {
            using T = std::remove_cvref_t<decltype(cmd)>;
            if constexpr (std::is_same_v<UsbIpResponse::UsbIpRetSubmit, T>) {
                uint32_t seqnum = cmd.header.seqnum;
                int64_t send_start_time = esp_timer_get_time();
                co_await cmd.to_socket_co(socket, sending_ec);
                if (!sending_ec) {
                    int64_t send_complete_time = esp_timer_get_time();
                    int64_t send_duration = send_complete_time - send_start_time;
                    
                    int64_t recv_time = 0;
                    {
                        std::shared_lock lock(timestamps_mutex_);
                        auto it = recv_timestamps_.find(seqnum);
                        if (it != recv_timestamps_.end()) {
                            recv_time = it->second;
                        }
                    }
                    
                    if (recv_time != 0) {
                        int64_t total_latency_us = send_complete_time - recv_time;
                        ESP_LOGI("NET_PERF", 
                                "Request seq=%u | 接收->处理完成: %lld us | 网络发送: %lld us | 总延迟: %lld us",
                                seqnum, 
                                send_start_time - recv_time,
                                send_duration,
                                total_latency_us);
                        total_latency_us_ += total_latency_us;
                        request_count_++;
                        
                        if (request_count_ % 100 == 0) {
                            double avg_latency = static_cast<double>(total_latency_us_) / request_count_;
                            ESP_LOGI("NET_PERF_AVG", 
                                    "Average latency for last %u requests: %.2f us (%.2f ms)",
                                    request_count_.load(), avg_latency, avg_latency / 1000.0);
                        }
                        
                        {
                            std::unique_lock lock(timestamps_mutex_);
                            recv_timestamps_.erase(seqnum);
                        }
                    }
                } else {
                    SPDLOG_ERROR("写入socket时出错 submit seq={} : {}", seqnum, sending_ec.message());
                }
            }
            else if constexpr (std::is_same_v<UsbIpResponse::UsbIpRetUnlink, T>) {
                uint32_t seqnum = cmd.header.seqnum;
                co_await cmd.to_socket_co(socket, sending_ec);
                if (sending_ec) {
                    SPDLOG_ERROR("写入socket时出错 unlink seq={} : {}", seqnum, sending_ec.message());
                }
            }
            else if constexpr (std::is_same_v<std::monostate, T>) {
                SPDLOG_ERROR("收到未知包");
                sending_ec = make_error_code(ErrorType::UNKNOWN_CMD);
            }
            else {
                static_assert(!std::is_same_v<T, T>);
            } }, send_data);

        if (sending_ec)
        {
            SPDLOG_ERROR("发送到 socket 时发生错误: {}", sending_ec.message());
            ec = sending_ec;
            break;
        }
    }

    if (ec == asio::experimental::error::channel_closed ||
        ec == asio::experimental::error::channel_cancelled)
    {
        SPDLOG_DEBUG("sender ec:{}", ec.message());
        ec.clear();
    }
    else if (ec)
    {
        SPDLOG_ERROR("sender exiting with ec: {}", ec.message());
    }
}