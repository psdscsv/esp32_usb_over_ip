#include "esp32_handler/Esp32DeviceHandler.h"

#include "sdkconfig.h"
#include <esp_log.h>

#include "Session.h"
#include "protocol.h"
#include "SetupPacket.h"
#include "constant.h"
#include "endpoint.h"
#include "protocol.h"
const char *usbipdcpp::Esp32DeviceHandler::TAG = "Esp32DeviceHandler";

usbipdcpp::Esp32DeviceHandler::Esp32DeviceHandler(UsbDevice &handle_device, usb_device_handle_t native_handle,
                                                  usb_host_client_handle_t host_client_handle) : DeviceHandlerBase(handle_device), native_handle(native_handle), host_client_handle(host_client_handle)
{
    ESP_ERROR_CHECK(usb_host_device_info(native_handle, &device_info));
}

usbipdcpp::Esp32DeviceHandler::~Esp32DeviceHandler()
{
}

void usbipdcpp::Esp32DeviceHandler::on_new_connection(Session &current_session, error_code &ec)
{
    session = &current_session;
    all_transfer_should_stop = false;
}

void usbipdcpp::Esp32DeviceHandler::on_disconnection(error_code &ec)
{
    all_transfer_should_stop = true;
    if (!has_device)
    {
        SPDLOG_WARN("没有设备，不需要停止传输");
        session = nullptr;
        return;
    }
    cancel_all_transfer();
    spdlog::info("成功取消所有传输");
    transferring_data.clear();
    concurrent_transfer_count = 0;
    session = nullptr;
}

void usbipdcpp::Esp32DeviceHandler::handle_unlink_seqnum(std::uint32_t seqnum)
{
    if (!has_device)
    {
        // 设备已经没了不可以再取消传输
        return;
    }
    cancel_all_transfer();
}
void usbipdcpp::Esp32DeviceHandler::handle_control_urb(
    std::uint32_t seqnum,
    const UsbEndpoint &ep,
    std::uint32_t transfer_flags,
    std::uint32_t transfer_buffer_length,
    const SetupPacket &setup_packet,
    const data_type &req,
    [[maybe_unused]] std::error_code &ec)
{
    if (!has_device)
    {
        ec = make_error_code(ErrorType::NO_DEVICE);
        return;
    }

    SPDLOG_DEBUG("控制请求: bmRequestType={:02x}, bRequest={}, wValue={}, wIndex={}, wLength={}",
                 setup_packet.request_type, setup_packet.request,
                 setup_packet.value, setup_packet.index, setup_packet.length);

    usb_transfer_t *transfer = nullptr;
    auto err = usb_host_transfer_alloc(USB_SETUP_PACKET_SIZE + transfer_buffer_length, 0, &transfer);
    if (err != ESP_OK)
    {
        SPDLOG_ERROR("无法申请transfer: %s", esp_err_to_name(err));
        ec = make_error_code(ErrorType::TRANSFER_ERROR);
        return;
    }

    // 设置 SETUP 包
    auto *setup_pkt = reinterpret_cast<usb_setup_packet_t *>(transfer->data_buffer);
    setup_pkt->bmRequestType = setup_packet.request_type;
    setup_pkt->bRequest = setup_packet.request;
    setup_pkt->wValue = setup_packet.value;
    setup_pkt->wIndex = setup_packet.index;
    setup_pkt->wLength = setup_packet.length;

    // 如果有 OUT 数据，复制到缓冲区
    if (setup_packet.is_out() && !req.empty())
    {
        if (req.size() <= transfer_buffer_length)
        {
            memcpy(transfer->data_buffer + USB_SETUP_PACKET_SIZE, req.data(), req.size());
        }
        else
        {
            SPDLOG_WARN("控制OUT数据大小{}超过缓冲区大小{}", req.size(), transfer_buffer_length);
        }
    }

    auto *callback_args = new (std::nothrow) esp32_callback_args{
        .handler = *this,
        .seqnum = seqnum,
        .transfer_type = USB_TRANSFER_TYPE_CTRL,
        .is_out = setup_packet.is_out(),
        .original_transfer_buffer_length = transfer_buffer_length,
        .counted_in_concurrent = false};

    if (!callback_args)
    {
        SPDLOG_ERROR("无法分配callback_args内存");
        usb_host_transfer_free(transfer);
        ec = make_error_code(ErrorType::TRANSFER_ERROR);
        return;
    }

    transfer->device_handle = native_handle;
    transfer->callback = transfer_callback;
    transfer->context = callback_args;
    transfer->bEndpointAddress = ep.address;
    transfer->num_bytes = USB_SETUP_PACKET_SIZE + setup_packet.length;
    transfer->flags = get_esp32_transfer_flags(transfer_flags);

    {
        std::lock_guard lock(transferring_data_mutex);
        transferring_data[seqnum] = transfer;
    }

    err = usb_host_transfer_submit_control(host_client_handle, transfer);
    if (err != ESP_OK)
    {
        SPDLOG_ERROR("transfer提交失败: %s", esp_err_to_name(err));
        {
            std::lock_guard lock(transferring_data_mutex);
            transferring_data.erase(seqnum);
        }
        usb_host_transfer_free(transfer);
        delete callback_args;

        session.load()->submit_ret_submit(
            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum));
    }
}

void usbipdcpp::Esp32DeviceHandler::handle_bulk_transfer(
    std::uint32_t seqnum,
    const UsbEndpoint &ep,
    UsbInterface &interface,
    std::uint32_t transfer_flags,
    std::uint32_t transfer_buffer_length,
    const data_type &out_data,
    [[maybe_unused]] std::error_code &ec)
{
    // 本函数已优化为完全异步传输：
    // - 对于可以一次发送的长度直接分配一个足够大的 transfer 并异步提交
    // - 只有在请求超过 USB_HOST_BULK_TRANSFER_MAX_SIZE 时使用并行拆分
    //   （该配置项可以通过 menuconfig 调整，推荐设置为 16K/32K/64K）
    // 这样就不会在主线程中串行等待 chunk，USB 和网络可以流水线并行。
    if (!has_device)
    {
        ec = make_error_code(ErrorType::NO_DEVICE);
        return;
    }

    check_and_clean_memory();

    // 检查并发传输数，如果达到上限则立即返回 EPIPE 给客户端。
    // 也可以在此处将请求排队再处理，但为避免无限制堆积导致内存炸裂，
    // 默认选择快速拒绝并由上层重试。
    if (concurrent_transfer_count >= MAX_CONCURRENT_TRANSFERS)
    {
        ESP_LOGW(TAG, "并发传输数达到限制(%zu)，返回EPIPE", static_cast<size_t>(MAX_CONCURRENT_TRANSFERS));
        session.load()->submit_ret_submit(
            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum));
        return;
    }

    concurrent_transfer_count++;
    bool is_out = !ep.is_in();

    // 最大内部传输长度由 SDK 配置项决定，可在 menuconfig 中调整
    // CONFIG_USB_HOST_BULK_TRANSFER_MAX_SIZE 默认可能较小，此处为安全回退
#ifdef CONFIG_USB_HOST_BULK_TRANSFER_MAX_SIZE
    constexpr uint32_t MAX_TRANSFER_SIZE = CONFIG_USB_HOST_BULK_TRANSFER_MAX_SIZE;
#else
    constexpr uint32_t MAX_TRANSFER_SIZE = 64 * 1024;
#endif

    // 请求长度不超过允许的最大值时直接异步提交一个 transfer
    uint32_t adjusted_length = std::min(transfer_buffer_length, MAX_TRANSFER_SIZE);

    // 对于 IN 传输，ESP32 USB Host 要求 num_bytes 为最大包大小的整数倍
    if (!is_out && ep.max_packet_size > 0)
    {
        if (adjusted_length % ep.max_packet_size != 0)
        {
            adjusted_length = ((adjusted_length + ep.max_packet_size - 1) / ep.max_packet_size) * ep.max_packet_size;
            adjusted_length = std::min(adjusted_length, MAX_TRANSFER_SIZE);
        }
    }

    // 如果传输请求大于单次最大值，则仍然尽量异步并行提交多个 transfer，
    // 但此路径应当很少触发——最好通过 menuconfig 将 MAX_TRANSFER_SIZE 提高到 32K/64K/128K。
    if (!is_out && transfer_buffer_length > MAX_TRANSFER_SIZE)
    {
        SPDLOG_WARN("请求长度 %u 超过 MAX_TRANSFER_SIZE=%u，将并行拆分", transfer_buffer_length, MAX_TRANSFER_SIZE);
        size_t remaining = transfer_buffer_length;
        auto aggregated = std::make_shared<data_type>();
        try
        {
            aggregated->resize(transfer_buffer_length);
        }
        catch (...)
        {
            SPDLOG_ERROR("无法为aggregated分配内存, size=%u, heap=%d", transfer_buffer_length, esp_get_free_heap_size());
            session.load()->submit_ret_submit(
                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum));
            concurrent_transfer_count--;
            return;
        }

        struct ChunkContext
        {
            Esp32DeviceHandler *handler;
            std::shared_ptr<data_type> agg;
            size_t offset;
            size_t length;
            std::atomic<size_t> *completed;
            std::atomic<usb_transfer_status_t> *last_status;
            uint32_t seqnum;
            size_t total_chunks;
            size_t original_length;
        };

        size_t total_chunks = (transfer_buffer_length + MAX_TRANSFER_SIZE - 1) / MAX_TRANSFER_SIZE;
        auto completed = std::make_shared<std::atomic<size_t>>(0);
        auto last_status = std::make_shared<std::atomic<usb_transfer_status_t>>(USB_TRANSFER_STATUS_COMPLETED);

        size_t offset = 0;
        for (size_t i = 0; i < total_chunks; ++i)
        {
            size_t this_len = std::min<size_t>(MAX_TRANSFER_SIZE, remaining);
            size_t submit_len = this_len;
            if (ep.max_packet_size > 0 && submit_len % ep.max_packet_size != 0)
            {
                submit_len = ((submit_len + ep.max_packet_size - 1) / ep.max_packet_size) * ep.max_packet_size;
            }

            usb_transfer_t *chunk_tr = nullptr;
            esp_err_t aerr = usb_host_transfer_alloc(submit_len, 0, &chunk_tr);
            if (aerr != ESP_OK)
            {
                SPDLOG_ERROR("chunk transfer alloc 失败: %s", esp_err_to_name(aerr));
                session.load()->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum));
                concurrent_transfer_count--;
                return;
            }

            auto ctx = new ChunkContext{this, aggregated, offset, this_len, completed.get(), last_status.get(), seqnum, total_chunks, transfer_buffer_length};

            chunk_tr->device_handle = native_handle;
            chunk_tr->callback = [](usb_transfer_t *trx)
            {
                auto ctx = static_cast<ChunkContext *>(trx->context);
                if (trx->status == USB_TRANSFER_STATUS_COMPLETED)
                {
                    size_t actual = static_cast<size_t>(trx->actual_num_bytes);
                    size_t to_copy = std::min(actual, ctx->length);
                    memcpy(ctx->agg->data() + ctx->offset, trx->data_buffer, to_copy);
                }
                else
                {
                    ctx->last_status->store(trx->status);
                }

                if (ctx->completed->fetch_add(1) + 1 == ctx->total_chunks)
                {
                    // 最后一个chunk完成，发送响应并减少并发计数
                    if (ctx->last_status->load() == USB_TRANSFER_STATUS_COMPLETED)
                    {
                        ctx->agg->resize(ctx->original_length);
                        ctx->handler->session.load()->submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit(
                                ctx->seqnum,
                                static_cast<uint32_t>(UrbStatusType::StatusOK),
                                0,
                                0,
                                ctx->agg,
                                {}));
                    }
                    else
                    {
                        ctx->handler->session.load()->submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit(
                                ctx->seqnum,
                                trxstat2error(ctx->last_status->load()),
                                0,
                                0,
                                ctx->agg,
                                {}));
                    }
                    ctx->handler->concurrent_transfer_count--;
                }
                usb_host_transfer_free(trx);
                delete ctx;
            };
            chunk_tr->context = ctx;
            chunk_tr->bEndpointAddress = ep.address;
            chunk_tr->num_bytes = submit_len;
            chunk_tr->flags = get_esp32_transfer_flags(transfer_flags);

            {
                std::lock_guard lock(endpoint_cancellation_mutex);
                aerr = usb_host_transfer_submit(chunk_tr);
            }
            if (aerr != ESP_OK)
            {
                SPDLOG_ERROR("chunk transfer 提交失败: %s", esp_err_to_name(aerr));
                usb_host_transfer_free(chunk_tr);
                session.load()->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum));
                concurrent_transfer_count--;
                return;
            }

            offset += this_len;
            remaining -= this_len;
        }
        return;
    }
    usb_transfer_t *transfer = nullptr;
    auto err = usb_host_transfer_alloc(adjusted_length, 0, &transfer);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "无法申请transfer: %s, 大小: %u", esp_err_to_name(err), adjusted_length);
        concurrent_transfer_count--;
        ec = make_error_code(ErrorType::TRANSFER_ERROR);
        return;
    }

    auto *callback_args = new (std::nothrow) esp32_callback_args{
        .handler = *this,
        .seqnum = seqnum,
        .transfer_type = USB_TRANSFER_TYPE_BULK,
        .is_out = is_out,
        .original_transfer_buffer_length = transfer_buffer_length,
        .counted_in_concurrent = true};

    if (!callback_args)
    {
        ESP_LOGE(TAG, "无法分配callback_args内存");
        usb_host_transfer_free(transfer);
        concurrent_transfer_count--;
        ec = make_error_code(ErrorType::TRANSFER_ERROR);
        return;
    }

    if (is_out && !out_data.empty())
    {
        size_t copy_size = std::min(out_data.size(), static_cast<size_t>(adjusted_length));
        memcpy(transfer->data_buffer, out_data.data(), copy_size);
    }

    transfer->device_handle = native_handle;
    transfer->callback = transfer_callback;
    transfer->context = callback_args;
    transfer->bEndpointAddress = ep.address;
    transfer->num_bytes = adjusted_length;
    transfer->flags = get_esp32_transfer_flags(transfer_flags);

    if (is_out)
    {
        transfer->flags &= USB_TRANSFER_FLAG_ZERO_PACK;
    }

    {
        std::lock_guard lock(transferring_data_mutex);
        transferring_data[seqnum] = transfer;
    }

    err = usb_host_transfer_submit(transfer);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "transfer提交失败: %s", esp_err_to_name(err));
        {
            std::lock_guard lock(transferring_data_mutex);
            transferring_data.erase(seqnum);
        }
        usb_host_transfer_free(transfer);
        delete callback_args;
        concurrent_transfer_count--;

        session.load()->submit_ret_submit(
            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum));
    }
}

void usbipdcpp::Esp32DeviceHandler::check_and_clean_memory()
{
    auto now = std::chrono::steady_clock::now();
    if (now - last_memory_check > std::chrono::seconds(30))
    {
        last_memory_check = now;

        int free_heap = esp_get_free_heap_size();
        ESP_LOGI(TAG, "内存状态: 空闲堆=%d, 并发传输=%zu", free_heap, concurrent_transfer_count.load());

        // 如果内存太低，强制清理
        if (free_heap < 10000)
        { // 10KB阈值
            ESP_LOGW(TAG, "内存不足，强制清理");
            cancel_all_transfer();

            // 清理transferring_data
            {
                std::lock_guard lock(transferring_data_mutex);
                for (auto &[seqnum, transfer] : transferring_data)
                {
                    if (transfer)
                    {
                        usb_host_transfer_free(transfer);
                    }
                }
                transferring_data.clear();
                concurrent_transfer_count = 0;
            }

            // 强制垃圾回收
            // heap_caps_print_heap_info(MALLOC_CAP_DEFAULT);
        }
    }
}

void usbipdcpp::Esp32DeviceHandler::handle_interrupt_transfer(std::uint32_t seqnum,
                                                              const UsbEndpoint &ep,
                                                              UsbInterface &interface,
                                                              std::uint32_t transfer_flags,
                                                              std::uint32_t transfer_buffer_length,
                                                              const data_type &out_data,
                                                              [[maybe_unused]] std::error_code &ec)
{
    if (!has_device)
    {
        ec = make_error_code(ErrorType::NO_DEVICE);
        return;
    }

    bool is_out = !ep.is_in();

    SPDLOG_DEBUG("中断传输 {}，ep addr: {:02x}", is_out ? "Out" : "In", ep.address);
    usb_transfer_t *transfer = nullptr;

    // 对于IN传输，ESP32 USB Host 要求 num_bytes 为最大包大小的整数倍，
    // 因此按 ep.max_packet_size 向上取整用于申请/提交，但保留 original length
    std::uint32_t adjusted_length = transfer_buffer_length;
    if (!is_out && ep.max_packet_size > 0)
    {
        if (adjusted_length % ep.max_packet_size != 0)
        {
            adjusted_length = ((adjusted_length + ep.max_packet_size - 1) / ep.max_packet_size) * ep.max_packet_size;
        }
    }

    auto err = usb_host_transfer_alloc(adjusted_length, 0, &transfer);
    {
        if (err != ESP_OK)
        {
            SPDLOG_ERROR("无法申请transfer");
            return;
        }
        if (is_out)
        {
            memcpy(transfer->data_buffer, out_data.data(), out_data.size());
        }
        auto *callback_args = new esp32_callback_args{
            .handler = *this,
            .seqnum = seqnum,
            .transfer_type = USB_TRANSFER_TYPE_INTR,
            .is_out = is_out,
            .original_transfer_buffer_length = transfer_buffer_length, // 保存原始长度
            .counted_in_concurrent = false};
        transfer->device_handle = native_handle;
        transfer->callback = transfer_callback;
        transfer->context = callback_args;
        transfer->bEndpointAddress = ep.address;
        transfer->num_bytes = adjusted_length; // 使用调整后的长度
        transfer->flags = get_esp32_transfer_flags(transfer_flags);

        {
            std::lock_guard lock(transferring_data_mutex);
            transferring_data[seqnum] = transfer;
        }

        err = usb_host_transfer_submit(transfer);

        if (err != ESP_OK)
        {
            SPDLOG_ERROR("transfer提交失败");
            usb_host_transfer_free(transfer);
            delete callback_args;
            goto error_occurred;
        }
    }
    return;
error_occurred:
    SPDLOG_ERROR("中断传输失败，{}", esp_err_to_name(err));
    // 不认为是错误，让服务器重置
    //  ec = make_error_code(ErrorType::TRANSFER_ERROR);
    session.load()->submit_ret_submit(
        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum));
}

void usbipdcpp::Esp32DeviceHandler::handle_isochronous_transfer(
    std::uint32_t seqnum,
    const UsbEndpoint &ep,
    UsbInterface &interface,
    std::uint32_t transfer_flags,
    std::uint32_t transfer_buffer_length,
    const data_type &req,
    const std::vector<UsbIpIsoPacketDescriptor> &
        iso_packet_descriptors,
    [[maybe_unused]] std::error_code &ec)
{
    if (!has_device)
    {
        ec = make_error_code(ErrorType::NO_DEVICE);
        return;
    }

    bool is_out = !ep.is_in();
    SPDLOG_DEBUG("同步传输 {}，ep addr: {:02x}", is_out ? "Out" : "In", ep.address);

    usb_transfer_t *transfer = nullptr;
    auto err = usb_host_transfer_alloc(transfer_buffer_length, iso_packet_descriptors.size(), &transfer);
    {
        if (err != ESP_OK)
        {
            SPDLOG_ERROR("无法申请transfer");
            ec = make_error_code(ErrorType::TRANSFER_ERROR);
            return;
        }
        if (is_out)
        {
            memcpy(transfer->data_buffer, req.data(), req.size());
        }
        auto *callback_args = new esp32_callback_args{
            .handler = *this,
            .seqnum = seqnum,
            .transfer_type = USB_TRANSFER_TYPE_ISOCHRONOUS,
            .is_out = is_out,
            .original_transfer_buffer_length = transfer_buffer_length, // 保存原始长度
            .counted_in_concurrent = false};
        transfer->device_handle = native_handle;
        transfer->callback = transfer_callback;
        transfer->context = callback_args;
        transfer->bEndpointAddress = ep.address;
        transfer->num_bytes = transfer_buffer_length;

        transfer->flags = get_esp32_transfer_flags(transfer_flags);

        for (std::size_t i = 0; i < iso_packet_descriptors.size(); i++)
        {
            auto &libusb_iso_desc_i = transfer->isoc_packet_desc[i];
            /* ignore iso->offset; */
            libusb_iso_desc_i.status = error2trxstat(iso_packet_descriptors[i].status);
            libusb_iso_desc_i.actual_num_bytes = iso_packet_descriptors[i].actual_length;
            libusb_iso_desc_i.num_bytes = iso_packet_descriptors[i].length;
        }

        transfer->flags = get_esp32_transfer_flags(transfer_flags);

        {
            std::lock_guard lock(transferring_data_mutex);
            transferring_data[seqnum] = transfer;
        }

        err = usb_host_transfer_submit(transfer);
        if (err < 0)
        {
            SPDLOG_ERROR("transfer提交失败");
            usb_host_transfer_free(transfer);
            delete callback_args;
            goto error_occurred;
        }
    }
    return;
error_occurred:
    SPDLOG_ERROR("同步传输失败，{}", esp_err_to_name(err));
    session.load()->submit_ret_submit(
        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum));
}

void usbipdcpp::Esp32DeviceHandler::cancel_all_transfer()
{

    // 0号端口不支持
    // cancel_endpoint_all_transfers(native_handle, 0x00);
    // cancel_endpoint_all_transfers(native_handle, 0x80);

    const usb_config_desc_t *config_desc;
    ESP_ERROR_CHECK(usb_host_get_active_config_descriptor(native_handle, &config_desc));
    const usb_intf_desc_t *intf = NULL;
    for (int i = 0; i < config_desc->bNumInterfaces; i++)
    {
        int intf_offset;
        intf = usb_parse_interface_descriptor(config_desc, i, 0, &intf_offset);
        if (!intf)
            continue;

        for (int j = 0; j < intf->bNumEndpoints; j++)
        {
            int endpoint_offset = intf_offset;
            const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(
                intf, j, config_desc->wTotalLength, &endpoint_offset);
            if (!ep)
                continue;
            // 清除当前端点的所有传输
            cancel_endpoint_all_transfers(ep->bEndpointAddress);
        }
    }
}

void usbipdcpp::Esp32DeviceHandler::cancel_endpoint_all_transfers(uint8_t bEndpointAddress)
{
    std::lock_guard lock(endpoint_cancellation_mutex);
    esp_err_t err;

    // 只执行必要的清理，避免过度操作
    err = usb_host_endpoint_clear(native_handle, bEndpointAddress);
    if (err != ESP_OK)
    {
        SPDLOG_WARN("usb_host_endpoint_clear address {} failed: {}",
                    bEndpointAddress, esp_err_to_name(err));
    }

    // 延迟后重置端点
    vTaskDelay(pdMS_TO_TICKS(10));
}

esp_err_t usbipdcpp::Esp32DeviceHandler::sync_control_transfer(const SetupPacket &setup_packet) const
{
    usb_transfer_t *transfer = nullptr;
    auto err = usb_host_transfer_alloc(USB_SETUP_PACKET_SIZE + setup_packet.length, 0, &transfer);
    if (err != ESP_OK)
    {
        SPDLOG_ERROR("无法申请transfer: %s", esp_err_to_name(err));
        return err;
    }

    auto setup_pkt = reinterpret_cast<usb_setup_packet_t *>(transfer->data_buffer);
    setup_pkt->bmRequestType = setup_packet.request_type;
    setup_pkt->bRequest = setup_packet.request;
    setup_pkt->wValue = setup_packet.value;
    setup_pkt->wIndex = setup_packet.index;
    setup_pkt->wLength = setup_packet.length;

    std::binary_semaphore semaphore{0};

    transfer->device_handle = native_handle;
    transfer->callback = [](usb_transfer_t *trx)
    {
        std::binary_semaphore &semaphore = *static_cast<std::binary_semaphore *>(trx->context);
        semaphore.release();
    };
    transfer->context = &semaphore;
    transfer->bEndpointAddress = setup_packet.calc_ep0_address();
    transfer->num_bytes = USB_SETUP_PACKET_SIZE + setup_packet.length;

    err = usb_host_transfer_submit_control(host_client_handle, transfer);
    if (err != ESP_OK)
    {
        SPDLOG_ERROR("sync_control_transfer 提交失败: %s", esp_err_to_name(err));
        usb_host_transfer_free(transfer);
        return err;
    }

    // 等待回调函数结束
    semaphore.acquire();

    // 传输完成后释放资源
    usb_host_transfer_free(transfer);
    return ESP_OK;
}

esp_err_t usbipdcpp::Esp32DeviceHandler::tweak_clear_halt_cmd(const SetupPacket &setup_packet)
{
    auto target_endp = setup_packet.index;
    SPDLOG_DEBUG("tweak_clear_halt_cmd");

    auto err = usb_host_endpoint_clear(native_handle, target_endp);
    if (err != ESP_OK)
    {
        SPDLOG_ERROR("tweak_clear_halt_cmd usb_host_endpoint_clear error: {}", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

esp_err_t usbipdcpp::Esp32DeviceHandler::tweak_set_interface_cmd(const SetupPacket &setup_packet)
{

    // 使用sync_control_transfer实际执行控制传输
    auto err = sync_control_transfer(setup_packet);
    if (err != ESP_OK)
    {
        SPDLOG_ERROR("error occurred in tweak_set_interface_cmd:{}", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

esp_err_t usbipdcpp::Esp32DeviceHandler::tweak_set_configuration_cmd(const SetupPacket &setup_packet)
{
    SPDLOG_DEBUG("tweak_set_configuration_cmd");

    // 不可以set_configuration，会device_busy
    // 就当执行过了
    return ESP_OK;
}

esp_err_t usbipdcpp::Esp32DeviceHandler::tweak_reset_device_cmd(const SetupPacket &setup_packet)
{
    SPDLOG_DEBUG("tweak_reset_device_cmd");

    // 使用sync_control_transfer实际执行控制传输
    auto err = sync_control_transfer(setup_packet);
    if (err != ESP_OK)
    {
        SPDLOG_ERROR("error occurred in tweak_reset_device_cmd:{}", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

bool usbipdcpp::Esp32DeviceHandler::tweak_special_requests(const SetupPacket &setup_packet)
{
    // 记录控制请求以便调试
    SPDLOG_DEBUG("控制请求: bmRequestType={:02x}, bRequest={}, wValue={}, wIndex={}, wLength={}",
                 setup_packet.request_type, setup_packet.request,
                 setup_packet.value, setup_packet.index, setup_packet.length);

    // 检查是否是标准设备请求
    if ((setup_packet.request_type & 0x60) == 0)
    { // 标准请求
        switch (setup_packet.request)
        {
        case 0x01: // CLEAR_FEATURE
            if (setup_packet.value == 0)
            { // ENDPOINT_HALT
                return tweak_clear_halt_cmd(setup_packet) == ESP_OK;
            }
            break;
        case 0x0B: // SET_INTERFACE
            // 对于U盘，set_interface通常是不需要的（只有一个接口设置）
            // 但有些U盘可能需要它
            SPDLOG_INFO("SET_INTERFACE请求: 接口={}, 备选设置={}",
                        setup_packet.index, setup_packet.value);
            // 返回false，让标准控制传输流程处理
            return false;
        case 0x09: // SET_CONFIGURATION
            SPDLOG_INFO("SET_CONFIGURATION请求: 配置值={}", setup_packet.value);
            // U盘通常只需要一个配置，直接返回成功
            return true;
        case 0x00: // GET_STATUS
        case 0x02: // SET_FEATURE
        case 0x03: // SET_ADDRESS
        case 0x06: // GET_DESCRIPTOR
        case 0x08: // GET_CONFIGURATION
        case 0x0A: // GET_INTERFACE
            // 这些请求应该由标准控制传输处理
            return false;
        default:
            SPDLOG_WARN("未知的标准请求: {}", setup_packet.request);
            return false;
        }
    }

    // 检查是否是类特定请求（U盘大容量存储类）
    if ((setup_packet.request_type & 0x60) == 0x20)
    { // 类特定请求
        SPDLOG_DEBUG("类特定请求");
        // 对于U盘，类特定请求应该由标准控制传输处理
        return false;
    }

    // 检查是否是供应商特定请求
    if ((setup_packet.request_type & 0x60) == 0x40)
    { // 供应商特定请求
        SPDLOG_DEBUG("供应商特定请求");
        return false;
    }

    SPDLOG_DEBUG("不需要调整包");
    return false;
}

uint8_t usbipdcpp::Esp32DeviceHandler::get_esp32_transfer_flags(uint32_t in)
{
    uint8_t flags = 0;

    // if (in & static_cast<std::uint32_t>(TransferFlag::URB_SHORT_NOT_OK))
    //     USB_TRANSFER_FLAG_ZERO_PACK
    //     flags |= LIBUSB_TRANSFER_SHORT_NOT_OK;
    if (in & static_cast<std::uint32_t>(TransferFlag::URB_ZERO_PACKET))
        flags |= USB_TRANSFER_FLAG_ZERO_PACK;

    return flags;
}

int usbipdcpp::Esp32DeviceHandler::trxstat2error(usb_transfer_status_t trxstat)
{
    switch (trxstat)
    {
    case USB_TRANSFER_STATUS_COMPLETED:
        return static_cast<int>(UrbStatusType::StatusOK);
    case USB_TRANSFER_STATUS_CANCELED:
        return static_cast<int>(UrbStatusType::StatusECONNRESET);
    case USB_TRANSFER_STATUS_ERROR:
    case USB_TRANSFER_STATUS_STALL:
    case USB_TRANSFER_STATUS_TIMED_OUT:
    case USB_TRANSFER_STATUS_OVERFLOW:
        return static_cast<int>(UrbStatusType::StatusEPIPE);
    case USB_TRANSFER_STATUS_NO_DEVICE:
        return static_cast<int>(UrbStatusType::StatusESHUTDOWN);
    default:
        return static_cast<int>(UrbStatusType::StatusENOENT);
    }
}

usb_transfer_status_t usbipdcpp::Esp32DeviceHandler::error2trxstat(int e)
{
    switch (e)
    {
    case static_cast<int>(UrbStatusType::StatusOK):
        return USB_TRANSFER_STATUS_COMPLETED;
    case static_cast<int>(UrbStatusType::StatusENOENT):
        return USB_TRANSFER_STATUS_ERROR;
    case static_cast<int>(UrbStatusType::StatusECONNRESET):
        return USB_TRANSFER_STATUS_CANCELED;
    case static_cast<int>(UrbStatusType::StatusETIMEDOUT):
        return USB_TRANSFER_STATUS_TIMED_OUT;
    case static_cast<int>(UrbStatusType::StatusEPIPE):
        return USB_TRANSFER_STATUS_STALL;
    case static_cast<int>(UrbStatusType::StatusESHUTDOWN):
        return USB_TRANSFER_STATUS_NO_DEVICE;
    case static_cast<int>(UrbStatusType::StatusEEOVERFLOW): // EOVERFLOW
        return USB_TRANSFER_STATUS_OVERFLOW;
    default:
        return USB_TRANSFER_STATUS_ERROR;
    }
}
void usbipdcpp::Esp32DeviceHandler::transfer_callback(usb_transfer_t *trx)
{
    auto callback_arg_ptr = static_cast<esp32_callback_args *>(trx->context);
    if (!callback_arg_ptr)
    {
        usb_host_transfer_free(trx);
        return;
    }

    auto &callback_arg = *callback_arg_ptr;

    callback_arg.handler.total_transfer_count++;

    if (callback_arg.handler.all_transfer_should_stop)
    {
        usb_host_transfer_free(trx);
        delete callback_arg_ptr;
        return;
    }

    {
        std::lock_guard lock(callback_arg.handler.transferring_data_mutex);
        callback_arg.handler.transferring_data.erase(callback_arg.seqnum);
    }

    auto unlink_found = callback_arg.handler.session.load()->get_unlink_seqnum(callback_arg.seqnum);
    bool should_send_response = true;

    size_t data_offset = 0;
    if (callback_arg.transfer_type == USB_TRANSFER_TYPE_CTRL && !callback_arg.is_out)
    {
        data_offset = USB_SETUP_PACKET_SIZE;
    }

    switch (trx->status)
    {
    case USB_TRANSFER_STATUS_COMPLETED:
        break;
    case USB_TRANSFER_STATUS_ERROR:
        SPDLOG_WARN("传输错误，端点: {:02x}", trx->bEndpointAddress);
        break;
    case USB_TRANSFER_STATUS_CANCELED:
    {
        if (!std::get<0>(unlink_found))
        {
            trx->status = USB_TRANSFER_STATUS_COMPLETED;
            esp_err_t err;
            {
                std::shared_lock lock(callback_arg.handler.endpoint_cancellation_mutex);
                if (callback_arg.transfer_type == USB_TRANSFER_TYPE_CTRL)
                {
                    err = usb_host_transfer_submit_control(callback_arg.handler.host_client_handle, trx);
                }
                else
                {
                    err = usb_host_transfer_submit(trx);
                }
            }
            if (err != ESP_OK)
            {
                SPDLOG_ERROR("重新提交失败 seq={}: {}", callback_arg.seqnum, esp_err_to_name(err));
                callback_arg.handler.session.load()->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(callback_arg.seqnum));
            }
            else
            {
                should_send_response = false;
            }
        }
        break;
    }
    case USB_TRANSFER_STATUS_STALL:
        SPDLOG_ERROR("端点 {:02x} 被STALL", trx->bEndpointAddress);
        break;
    case USB_TRANSFER_STATUS_NO_DEVICE:
        callback_arg.handler.has_device = false;
        SPDLOG_INFO("设备已移除");
        break;
    case USB_TRANSFER_STATUS_TIMED_OUT:
        SPDLOG_WARN("传输超时，端点: {:02x}", trx->bEndpointAddress);
        break;
    case USB_TRANSFER_STATUS_OVERFLOW:
        SPDLOG_WARN("传输溢出，端点: {:02x}", trx->bEndpointAddress);
        break;
    default:
        SPDLOG_WARN("未知的传输状态 {}", (int)trx->status);
        break;
    }

    if (should_send_response && !std::get<0>(unlink_found))
    {
        if (!callback_arg.is_out && trx->actual_num_bytes > static_cast<int>(data_offset))
        {
            callback_arg.handler.zero_copy_count++;

            SPDLOG_TRACE("零拷贝响应: seq={}, 实际长度={}, 偏移={}, 总计零拷贝={}",
                         callback_arg.seqnum,
                         trx->actual_num_bytes - data_offset,
                         data_offset,
                         callback_arg.handler.zero_copy_count.load());

            // 使用新的工厂函数创建零拷贝响应
            auto response = UsbIpResponse::UsbIpRetSubmit::create_ret_submit(
                callback_arg.seqnum,
                trxstat2error(trx->status),
                0,
                0,
                UsbTransferPtr(trx), // 转移所有权
                data_offset,
                {});

            callback_arg.handler.session.load()->submit_ret_submit(std::move(response));
            // 注意：trx 已经被转移所有权，不能调用 usb_host_transfer_free
        }
        else
        {
            data_type response_data;
            if (!callback_arg.is_out && trx->actual_num_bytes > static_cast<int>(data_offset))
            {
                size_t actual_data_len = trx->actual_num_bytes - data_offset;
                actual_data_len = std::min(actual_data_len,
                                           static_cast<size_t>(callback_arg.original_transfer_buffer_length));

                if (actual_data_len > 0)
                {
                    response_data.assign(
                        trx->data_buffer + data_offset,
                        trx->data_buffer + data_offset + actual_data_len);
                }
            }

            callback_arg.handler.session.load()->submit_ret_submit(
                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                    callback_arg.seqnum,
                    trxstat2error(trx->status),
                    response_data));

            usb_host_transfer_free(trx);
        }
    }
    else if (should_send_response)
    {
        auto cmd_unlink_seqnum = std::get<1>(unlink_found);

        callback_arg.handler.session.load()->submit_ret_unlink_and_then_remove_seqnum_unlink(
            UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
                cmd_unlink_seqnum,
                trxstat2error(trx->status)),
            callback_arg.seqnum);

        usb_host_transfer_free(trx);
    }
    else
    {
        usb_host_transfer_free(trx);
    }

    if (callback_arg.counted_in_concurrent)
    {
        callback_arg.handler.concurrent_transfer_count--;
    }

    delete callback_arg_ptr;
}