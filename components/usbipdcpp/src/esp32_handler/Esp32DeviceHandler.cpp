#include "esp32_handler/Esp32DeviceHandler.h"

#include <esp_log.h>

#include "Session.h"
#include "protocol.h"
#include "SetupPacket.h"
#include "constant.h"
#include "endpoint.h"

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
    const SetupPacket &setup_packet, const data_type &req,
    [[maybe_unused]] std::error_code &ec)
{
    if (!has_device)
    {
        ec = make_error_code(ErrorType::NO_DEVICE);
        return;
    }

    // 记录控制请求
    SPDLOG_DEBUG("控制请求: bmRequestType={:02x}, bRequest={}, wValue={}, wIndex={}, wLength={}",
                 setup_packet.request_type, setup_packet.request,
                 setup_packet.value, setup_packet.index, setup_packet.length);
    // 其他控制请求正常处理
    SPDLOG_DEBUG("控制传输 {}，ep addr: {:02x}",
                 ep.direction() == UsbEndpoint::Direction::Out ? "Out" : "In",
                 ep.address);

    usb_transfer_t *transfer = nullptr;
    auto err = usb_host_transfer_alloc(USB_SETUP_PACKET_SIZE + transfer_buffer_length, 0, &transfer);
    if (err != ESP_OK)
    {
        SPDLOG_ERROR("无法申请transfer: %s", esp_err_to_name(err));
        ec = make_error_code(ErrorType::TRANSFER_ERROR);
        return;
    }

    // 设置SETUP包
    auto *setup_pkt = reinterpret_cast<usb_setup_packet_t *>(transfer->data_buffer);
    setup_pkt->bmRequestType = setup_packet.request_type;
    setup_pkt->bRequest = setup_packet.request;
    setup_pkt->wValue = setup_packet.value;
    setup_pkt->wIndex = setup_packet.index;
    setup_pkt->wLength = setup_packet.length;

    // 如果有数据，复制到缓冲区
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

        // 返回错误响应
        session.load()->submit_ret_submit(
            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum));
        return;
    }
}
void usbipdcpp::Esp32DeviceHandler::handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
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
    check_and_clean_memory();
    // 检查并发传输数
    if (concurrent_transfer_count >= MAX_CONCURRENT_TRANSFERS)
    {
        ESP_LOGW(TAG, "并发传输数达到限制(%zu)，等待", MAX_CONCURRENT_TRANSFERS);
        // 返回忙状态，让客户端重试
        session.load()->submit_ret_submit(
            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum));
        return;
    }

    concurrent_transfer_count++;
    bool is_out = !ep.is_in();

    // 若需要更稳定的长传输，请考虑流式发送或更严格的并发控制
    const uint32_t MAX_TRANSFER_SIZE = 64 * 1024; // 最大传输
    uint32_t adjusted_length = std::min(transfer_buffer_length, MAX_TRANSFER_SIZE);

    // 对于IN传输，ESP32 USB Host 要求 num_bytes 为最大包大小的整数倍，
    // 因此这里按 ep.max_packet_size 向上取整用于申请/提交，但我们将
    // `original_transfer_buffer_length` 保留为主机请求的 `transfer_buffer_length`，
    // 回应时只返回实际需要的数据以保留短包语义。
    if (!is_out && ep.max_packet_size > 0)
    {
        if (adjusted_length % ep.max_packet_size != 0)
        {
            adjusted_length = ((adjusted_length + ep.max_packet_size - 1) / ep.max_packet_size) * ep.max_packet_size;
            adjusted_length = std::min(adjusted_length, MAX_TRANSFER_SIZE);
        }
    }

    // 如果请求很大（例如 64KB），一次性分配可能会导致内存不足。
    // 对于 IN（设备->主机）传输，我们按小块（CHUNK_SIZE）同步读取并合并结果，
    // 避免一次性分配过大的缓冲区导致 std::bad_alloc。
    const std::size_t CHUNK_SIZE = 32 * 1024; // 8KB
    if (!is_out && transfer_buffer_length > CHUNK_SIZE)
    {
        // ESP_LOGI(TAG, "大请求走分片路径: seqnum=%u, total_len=%u, CHUNK_SIZE=%u, heap=%d",seqnum, transfer_buffer_length, CHUNK_SIZE, esp_get_free_heap_size());
        //  heap_caps_print_heap_info(MALLOC_CAP_DEFAULT);

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
            return;
        }
        size_t write_offset = 0;
        usb_transfer_status_t last_status = USB_TRANSFER_STATUS_COMPLETED;

        while (remaining > 0)
        {
            size_t this_chunk = std::min<std::size_t>(CHUNK_SIZE, remaining);
            // 对齐到max_packet_size（如果有）
            if (ep.max_packet_size > 0 && this_chunk % ep.max_packet_size != 0)
            {
                this_chunk = ((this_chunk + ep.max_packet_size - 1) / ep.max_packet_size) * ep.max_packet_size;
            }

            usb_transfer_t *chunk_tr = nullptr;
            esp_err_t aerr = usb_host_transfer_alloc(this_chunk, 0, &chunk_tr);
            if (aerr != ESP_OK)
            {
                SPDLOG_ERROR("无法申请chunk transfer: %s, 尝试大小: %u, heap=%d", esp_err_to_name(aerr), (unsigned)this_chunk, esp_get_free_heap_size());
                heap_caps_print_heap_info(MALLOC_CAP_DEFAULT);
                // 内存分配失败，返回错误给客户端
                session.load()->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum));
                return;
            }

            std::binary_semaphore sem{0};
            chunk_tr->device_handle = native_handle;
            chunk_tr->callback = [](usb_transfer_t *trx)
            {
                std::binary_semaphore &sem = *static_cast<std::binary_semaphore *>(trx->context);
                sem.release();
            };
            chunk_tr->context = &sem;
            chunk_tr->bEndpointAddress = ep.address;
            chunk_tr->num_bytes = static_cast<int>(this_chunk);
            chunk_tr->flags = get_esp32_transfer_flags(transfer_flags);

            {
                std::shared_lock lock(endpoint_cancellation_mutex);
                aerr = usb_host_transfer_submit(chunk_tr);
            }
            if (aerr != ESP_OK)
            {
                SPDLOG_ERROR("chunk transfer 提交失败: %s", esp_err_to_name(aerr));
                usb_host_transfer_free(chunk_tr);
                session.load()->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum));
                return;
            }

            // 等待本次chunk完成
            sem.acquire();

            // 处理状态
            if (chunk_tr->status == USB_TRANSFER_STATUS_COMPLETED)
            {
                size_t actual = static_cast<size_t>(chunk_tr->actual_num_bytes);
                if (actual > 0)
                {
                    size_t to_copy = std::min(actual, remaining);
                    memcpy(aggregated->data() + write_offset, chunk_tr->data_buffer, to_copy);
                    write_offset += to_copy;
                    remaining -= to_copy;
                }
                else
                {
                    // 短包或没有数据，结束读取
                    remaining = 0;
                }
            }
            else
            {
                last_status = chunk_tr->status;
                // 发生错误，返回当前错误状态
                usb_host_transfer_free(chunk_tr);
                session.load()->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit(
                        seqnum,
                        trxstat2error(last_status),
                        0,
                        0,
                        aggregated,
                        {}));
                return;
            }

            usb_host_transfer_free(chunk_tr);

            // 如果收到的实际字节少于请求的chunk大小，说明设备发送短包，结束读取
            if (chunk_tr->actual_num_bytes < static_cast<int>(this_chunk))
            {
                break;
            }
        }

        // 所有chunk读取完成，发送一次合并的响应
        session.load()->submit_ret_submit(
            UsbIpResponse::UsbIpRetSubmit::create_ret_submit(
                seqnum,
                static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                0,
                0,
                aggregated,
                {}));
        return;
    }

    // ESP_LOGI(TAG, "块传输请求: seqnum=%u, len=%u->%u, heap=%d",seqnum, transfer_buffer_length, adjusted_length, esp_get_free_heap_size());

    usb_transfer_t *transfer = nullptr;
    // ESP_LOGI(TAG, "申请单次transfer: seqnum=%u, adjusted_length=%u, heap=%d", seqnum, adjusted_length, esp_get_free_heap_size());
    // heap_caps_print_heap_info(MALLOC_CAP_DEFAULT);

    auto err = usb_host_transfer_alloc(adjusted_length, 0, &transfer);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "无法申请transfer: %s, 需要大小: %u, heap=%d", esp_err_to_name(err), adjusted_length, esp_get_free_heap_size());
        heap_caps_print_heap_info(MALLOC_CAP_DEFAULT);
        ec = make_error_code(ErrorType::TRANSFER_ERROR);
        return;
    }

    auto *callback_args = new (std::nothrow) esp32_callback_args{
        .handler = *this,
        .seqnum = seqnum,
        .transfer_type = USB_TRANSFER_TYPE_BULK,
        .is_out = is_out,
        .original_transfer_buffer_length = transfer_buffer_length, // 保留主机请求长度
        .counted_in_concurrent = true};

    if (!callback_args)
    {
        ESP_LOGE(TAG, "无法分配callback_args内存");
        usb_host_transfer_free(transfer);
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

        // 返回错误响应
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

    // 记录内存使用情况
    static int callback_count = 0;
    if (++callback_count % 10 == 0)
    {
        // ESP_LOGI(TAG, "transfer_callback #%d, heap: %d, trx_size: %d",callback_count, esp_get_free_heap_size(), trx->num_bytes);
    }

    if (callback_arg.handler.all_transfer_should_stop)
    {
        usb_host_transfer_free(trx);
        delete callback_arg_ptr;
        return;
    }

    // 调了回调则当前包并未在发送，因此只要调了回调就先将其删了
    {
        std::lock_guard lock(callback_arg.handler.transferring_data_mutex);
        callback_arg.handler.transferring_data.erase(callback_arg.seqnum);
    }

    auto unlink_found = callback_arg.handler.session.load()->get_unlink_seqnum(callback_arg.seqnum);

    // 处理传输状态
    bool should_send_response = true;
    std::unique_ptr<data_type> received_data_ptr;
    std::unique_ptr<std::vector<UsbIpIsoPacketDescriptor>> iso_packet_descriptors_ptr;

    switch (trx->status)
    {
    case USB_TRANSFER_STATUS_COMPLETED:
        /* OK */
        break;
    case USB_TRANSFER_STATUS_ERROR:
        SPDLOG_WARN("传输错误，端点: {:02x}", trx->bEndpointAddress);
        break;
    case USB_TRANSFER_STATUS_CANCELED:
    {
        if (!std::get<0>(unlink_found))
        {
            // 取消的不是自己，重新提交自己
            trx->status = USB_TRANSFER_STATUS_COMPLETED;
            esp_err_t err;
            {
                std::shared_lock lock(callback_arg.handler.endpoint_cancellation_mutex);
                if (callback_arg.transfer_type == USB_TRANSFER_TYPE_CTRL)
                {
                    SPDLOG_TRACE("尝试重新提交控制传输");
                    err = usb_host_transfer_submit_control(callback_arg.handler.host_client_handle, trx);
                }
                else
                {
                    SPDLOG_TRACE("尝试重新提交非控制传输");
                    err = usb_host_transfer_submit(trx);
                }
            }
            if (err != ESP_OK)
            {
                SPDLOG_ERROR("seqnum为{}的传输重新提交失败：{}", callback_arg.seqnum, esp_err_to_name(err));
                // 提交epipe
                callback_arg.handler.session.load()->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(
                        callback_arg.seqnum));
            }
            else
            {
                should_send_response = false; // 重新提交了，不要发送响应
            }
        }
        break;
    }
    case USB_TRANSFER_STATUS_STALL:
        SPDLOG_ERROR("端点 {:02x} 被STALL", trx->bEndpointAddress);
        break;
    case USB_TRANSFER_STATUS_NO_DEVICE:
    {
        callback_arg.handler.has_device = false;
        SPDLOG_INFO("设备已移除");
        break;
    }
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

    // 如果需要发送响应，准备数据
    if (should_send_response && !std::get<0>(unlink_found))
    {
        data_type response_data;
        std::vector<UsbIpIsoPacketDescriptor> iso_descriptors;

        if (!callback_arg.is_out)
        {
            size_t data_offset = 0;
            if (callback_arg.transfer_type == USB_TRANSFER_TYPE_CTRL)
            {
                data_offset = USB_SETUP_PACKET_SIZE;
            }

            // 计算实际应该返回的数据长度
            size_t actual_data_len = 0;
            if (trx->actual_num_bytes > data_offset)
            {
                actual_data_len = trx->actual_num_bytes - data_offset;
                // 不能超过客户端请求的长度
                actual_data_len = std::min(actual_data_len,
                                           static_cast<size_t>(callback_arg.original_transfer_buffer_length));

                if (actual_data_len > 0)
                {
                    response_data.reserve(actual_data_len);
                    response_data.assign(
                        trx->data_buffer + data_offset,
                        trx->data_buffer + data_offset + actual_data_len);
                }
            }

            // ESP_LOGI(TAG, "返回数据: seq=%u, 实际长度=%zu, 请求长度=%u", callback_arg.seqnum, actual_data_len, callback_arg.original_transfer_buffer_length);
        }

        // 发送响应
        try
        {
            callback_arg.handler.session.load()->submit_ret_submit(
                UsbIpResponse::UsbIpRetSubmit::create_ret_submit(
                    callback_arg.seqnum,
                    trxstat2error(trx->status),
                    0,
                    trx->num_isoc_packets,
                    std::move(response_data),
                    std::move(iso_descriptors)));
        }
        catch (const std::exception &e)
        {
            ESP_LOGE(TAG, "发送响应失败: %s", e.what());
        }
    }

    else if (should_send_response)
    {
        auto cmd_unlink_seqnum = std::get<1>(unlink_found);

        // 发送ret_unlink
        try
        {
            SPDLOG_INFO("发送 ret_unlink: cmd_unlink_seqnum={}, seq={}, status={}",
                        cmd_unlink_seqnum, callback_arg.seqnum, trxstat2error(trx->status));
            callback_arg.handler.session.load()->submit_ret_unlink_and_then_remove_seqnum_unlink(
                UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
                    cmd_unlink_seqnum,
                    trxstat2error(trx->status)),
                callback_arg.seqnum);
        }
        catch (const std::bad_alloc &e)
        {
            ESP_LOGE(TAG, "submit_ret_unlink_and_then_remove_seqnum_unlink 内存分配失败: %s", e.what());
        }
    }

    // 清理数据 - 使用智能指针确保释放
    if (callback_arg.counted_in_concurrent)
    {
        size_t expected = callback_arg.handler.concurrent_transfer_count.load();
        while (expected > 0)
        {
            if (callback_arg.handler.concurrent_transfer_count.compare_exchange_weak(expected, expected - 1))
                break;
            // compare_exchange_weak updates expected on failure
        }
    }
    usb_host_transfer_free(trx);
    delete callback_arg_ptr;

    // 强制清理内存
    if (received_data_ptr)
    {
        received_data_ptr->clear();
        received_data_ptr->shrink_to_fit();
    }
    if (iso_packet_descriptors_ptr)
    {
        iso_packet_descriptors_ptr->clear();
        iso_packet_descriptors_ptr->shrink_to_fit();
    }
}