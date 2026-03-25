#pragma once

#include <map>
#include <shared_mutex>
#include <atomic>
#include <chrono>
#include <memory>

#include <asio.hpp>
#include <usb/usb_host.h>

#include "DeviceHandler/DeviceHandler.h"
#include "SetupPacket.h"
#include "esp32_handler/tools.h"
#include "esp32_handler/ConcurrentTransferTracker.h"
#include "esp_timer.h"

namespace usbipdcpp
{
    class Esp32DeviceHandler : public DeviceHandlerBase
    {
        friend class Esp32Server;

    public:
        Esp32DeviceHandler(UsbDevice &handle_device, usb_device_handle_t native_handle,
                           usb_host_client_handle_t host_client_handle);

        ~Esp32DeviceHandler() override;

        void on_new_connection(Session &current_session, error_code &ec) override;
        void on_disconnection(error_code &ec) override;
        void handle_unlink_seqnum(std::uint32_t seqnum) override;

    protected:
        void handle_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                const SetupPacket &setup_packet, const data_type &req, std::error_code &ec) override;
        /**
         * Bulk 转输。已经改为完全异步：
         * - 请求长度 <= CONFIG_USB_HOST_BULK_TRANSFER_MAX_SIZE 时一次性提交一个
         *   transfer
         * - 超出则并行拆分多个 transfer 并在最后统一响应
         * 这样避免了早期实现中的同步分片等待，使 USB 和网络数据可以流水线并行。
         */
        void handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                  UsbInterface &interface, std::uint32_t transfer_flags,
                                  std::uint32_t transfer_buffer_length, const data_type &out_data,
                                  std::error_code &ec) override;
        void handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                       UsbInterface &interface, std::uint32_t transfer_flags,
                                       std::uint32_t transfer_buffer_length, const data_type &out_data,
                                       std::error_code &ec) override;

        void handle_isochronous_transfer(std::uint32_t seqnum,
                                         const UsbEndpoint &ep, UsbInterface &interface,
                                         std::uint32_t transfer_flags,
                                         std::uint32_t transfer_buffer_length,
                                         const data_type &req,
                                         const std::vector<UsbIpIsoPacketDescriptor> &iso_packet_descriptors,
                                         std::error_code &ec) override;
        void cancel_all_transfer();
        void cancel_endpoint_all_transfers(uint8_t bEndpointAddress);

        // 防止还没结束恢复端点状态就重新提交导致状态错误
        std::shared_mutex endpoint_cancellation_mutex;

        /**
         * @brief 发生错误代表没成功传输，设备未收到消息
         * @param setup_packet
         * @return
         */
        esp_err_t sync_control_transfer(const SetupPacket &setup_packet) const;

        esp_err_t tweak_clear_halt_cmd(const SetupPacket &setup_packet);
        esp_err_t tweak_set_interface_cmd(const SetupPacket &setup_packet);
        esp_err_t tweak_set_configuration_cmd(const SetupPacket &setup_packet);
        esp_err_t tweak_reset_device_cmd(const SetupPacket &setup_packet);

        /**
         * @brief 返回是否做了特殊操作
         * @param setup_packet
         * @return
         */
        bool tweak_special_requests(const SetupPacket &setup_packet);

        static uint8_t get_esp32_transfer_flags(uint32_t in);

        static int trxstat2error(usb_transfer_status_t trxstat);
        static usb_transfer_status_t error2trxstat(int e);

        struct esp32_callback_args
        {
            Esp32DeviceHandler &handler;
            std::uint32_t seqnum;
            usb_transfer_type_t transfer_type;
            bool is_out;
            std::uint32_t original_transfer_buffer_length;
            bool counted_in_concurrent = false;

            uint64_t recv_time;   // 收到网络请求的时间
            uint64_t submit_time; // USB传输提交的时间
        };

        static void transfer_callback(usb_transfer_t *trx);

        static const char *TAG;

        // 优化：使用分段锁追踪器替代大锁
        ConcurrentTransferTracker transfer_tracker_;

        usb_device_handle_t native_handle;
        usb_device_info_t device_info{};
        usb_host_client_handle_t host_client_handle;

        std::atomic_bool all_transfer_should_stop = true;
        std::atomic_bool has_device = true;

    private:
        // 内存监控
        void check_and_clean_memory();
        std::chrono::steady_clock::time_point last_memory_check;

        // 最大并发传输数限制，通过 concurrent_transfer_count
        std::atomic<size_t> concurrent_transfer_count{0};

        // 统计零拷贝传输次数
        std::atomic<uint32_t> zero_copy_count{0};
        std::atomic<uint32_t> total_transfer_count{0};
    };
}