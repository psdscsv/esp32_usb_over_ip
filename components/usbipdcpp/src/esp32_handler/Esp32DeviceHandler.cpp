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

    // 对于标准设备请求，我们不应该拦截，让它们正常执行
    bool should_intercept = false;

    // 只拦截我们知道会有问题的请求
    if (setup_packet.is_set_configuration_cmd())
    {
        // SET_CONFIGURATION 请求对于U盘是必要的，不能拦截
        // 但是ESP32可能已经设置了配置，我们可以模拟成功
        if (setup_packet.value == 1)
        { // 配置值为1（通常的配置值）
            SPDLOG_INFO("模拟SET_CONFIGURATION请求成功，配置值=1");
            should_intercept = true;
        }
    }

    if (should_intercept)
    {
        SPDLOG_INFO("拦截了控制包：{}", seqnum);
        session.load()->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit(
            seqnum,
            static_cast<std::uint32_t>(UrbStatusType::StatusOK),
            0,
            0,
            {},
            {}));
        return;
    }

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
        .original_transfer_buffer_length = transfer_buffer_length};

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

    bool is_out = !ep.is_in();

    SPDLOG_DEBUG("块传输 {}，ep addr: {:02x}", is_out ? "Out" : "In", ep.address);
    usb_transfer_t *transfer = nullptr;

    // 对于IN传输，确保传输长度是最大包大小的整数倍
    std::uint32_t adjusted_length = transfer_buffer_length;
    if (!is_out && ep.max_packet_size > 0)
    {
        // 如果原始长度不是最大包大小的整数倍，调整到下一个整数倍
        if (transfer_buffer_length % ep.max_packet_size != 0)
        {
            adjusted_length = ((transfer_buffer_length / ep.max_packet_size) + 1) * ep.max_packet_size;
            SPDLOG_DEBUG("调整IN传输长度: {} -> {}, max_packet_size: {}",
                         transfer_buffer_length, adjusted_length, ep.max_packet_size);
        }
    }

    auto err = usb_host_transfer_alloc(adjusted_length, 0, &transfer);
    {
        if (err != ESP_OK)
        {
            SPDLOG_ERROR("无法申请transfer");
            ec = make_error_code(ErrorType::TRANSFER_ERROR);
            return;
        }
        if (is_out)
        {
            memcpy(transfer->data_buffer, out_data.data(), out_data.size());
        }
        auto *callback_args = new esp32_callback_args{
            .handler = *this,
            .seqnum = seqnum,
            .transfer_type = USB_TRANSFER_TYPE_BULK,
            .is_out = is_out,
            .original_transfer_buffer_length = transfer_buffer_length // 保存原始长度
        };
        transfer->device_handle = native_handle;
        transfer->callback = transfer_callback;
        transfer->context = callback_args;
        transfer->bEndpointAddress = ep.address;
        transfer->num_bytes = adjusted_length; // 使用调整后的长度
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
            SPDLOG_ERROR("transfer提交失败");
            usb_host_transfer_free(transfer);
            delete callback_args;
            goto error_occurred;
        }
    }
    return;
error_occurred:
    SPDLOG_ERROR("块传输失败，{}", esp_err_to_name(err));
    session.load()->submit_ret_submit(
        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum));
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

    // 对于IN传输，确保传输长度是最大包大小的整数倍
    std::uint32_t adjusted_length = transfer_buffer_length;
    if (!is_out && ep.max_packet_size > 0)
    {
        // 如果原始长度不是最大包大小的整数倍，调整到下一个整数倍
        if (transfer_buffer_length % ep.max_packet_size != 0)
        {
            adjusted_length = ((transfer_buffer_length / ep.max_packet_size) + 1) * ep.max_packet_size;
            SPDLOG_DEBUG("调整IN传输长度: {} -> {}, max_packet_size: {}",
                         transfer_buffer_length, adjusted_length, ep.max_packet_size);
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
            .original_transfer_buffer_length = transfer_buffer_length // 保存原始长度
        };
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
            .original_transfer_buffer_length = transfer_buffer_length // 保存原始长度
        };
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
    err = usb_host_endpoint_halt(native_handle, bEndpointAddress);
    if (err != ESP_OK)
    {
        SPDLOG_ERROR("usb_host_endpoint_halt address {} failed: {}", bEndpointAddress, esp_err_to_name(err));
    }
    err = usb_host_endpoint_flush(native_handle, bEndpointAddress);
    if (err != ESP_OK)
    {
        SPDLOG_ERROR("usb_host_endpoint_flush address {} failed: {}", bEndpointAddress, esp_err_to_name(err));
    }
    err = usb_host_endpoint_clear(native_handle, bEndpointAddress);
    if (err != ESP_OK)
    {
        SPDLOG_ERROR("usb_host_endpoint_clear address {} failed: {}", bEndpointAddress, esp_err_to_name(err));
    }
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
    uint16_t alternate = setup_packet.value;
    uint16_t interface = setup_packet.index;

    SPDLOG_DEBUG("set_interface: inf {} alt {}",
                 interface, alternate);

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
    auto &callback_arg = *static_cast<esp32_callback_args *>(trx->context);
    if (callback_arg.handler.all_transfer_should_stop)
    {
        // 清理资源
        usb_host_transfer_free(trx);
        delete &callback_arg;
        return;
    }

    // 调了回调则当前包并未在发送，因此只要调了回调就先将其删了
    {
        std::lock_guard lock(callback_arg.handler.transferring_data_mutex);
        callback_arg.handler.transferring_data.erase(callback_arg.seqnum);
    }

    auto unlink_found = callback_arg.handler.session.load()->get_unlink_seqnum(callback_arg.seqnum);

    // 检查内存状态
    static int memory_check_counter = 0;
    if (++memory_check_counter % 100 == 0)
    {
        ESP_LOGI(TAG, "transfer_callback 当前堆内存: %d", esp_get_free_heap_size());
        memory_check_counter = 0;
    }

    // 处理传输状态
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
                // 清理资源
                usb_host_transfer_free(trx);
                delete &callback_arg;
            }
            return; // 不要继续处理，等待下次回调
        }
    }
    break;
    case USB_TRANSFER_STATUS_STALL:
        SPDLOG_ERROR("端点 {:02x} 被STALL", trx->bEndpointAddress);
        break;
    case USB_TRANSFER_STATUS_NO_DEVICE:
    {
        callback_arg.handler.has_device = false;
        SPDLOG_INFO("设备已移除");

        callback_arg.handler.session.load()->submit_ret_submit(
            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(
                callback_arg.seqnum));
        // 清理数据
        usb_host_transfer_free(trx);
        delete &callback_arg;
        return;
    }
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

    // 处理传输数据
    if (!std::get<0>(unlink_found))
    {
        // 发送ret_submit
        data_type received_data;
        std::vector<UsbIpIsoPacketDescriptor> iso_packet_descriptors{};

        if (!callback_arg.is_out)
        {
            if (callback_arg.transfer_type == USB_TRANSFER_TYPE_CTRL)
            {
                size_t data_offset = USB_SETUP_PACKET_SIZE;
                size_t max_data_length = 0;

                if (trx->actual_num_bytes > data_offset)
                {
                    max_data_length = std::min(
                        static_cast<size_t>(callback_arg.original_transfer_buffer_length),
                        static_cast<size_t>(trx->actual_num_bytes - data_offset));

                    // 预分配内存避免重新分配
                    received_data.reserve(max_data_length);
                    received_data.assign(
                        trx->data_buffer + data_offset,
                        trx->data_buffer + data_offset + max_data_length);
                }
            }
            else if (callback_arg.transfer_type == USB_TRANSFER_TYPE_ISOCHRONOUS)
            {
                // 等时传输处理（U盘通常不使用）
                // ... 保持原有代码
            }
            else
            {
                // 块传输和中断传输
                size_t max_data_length = std::min(
                    static_cast<size_t>(callback_arg.original_transfer_buffer_length),
                    static_cast<size_t>(trx->actual_num_bytes));

                // 预分配内存避免重新分配
                received_data.reserve(max_data_length);
                received_data.assign(
                    trx->data_buffer,
                    trx->data_buffer + max_data_length);
            }
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
                    std::move(received_data),
                    std::move(iso_packet_descriptors)));
        }
        catch (const std::bad_alloc &e)
        {
            ESP_LOGE(TAG, "submit_ret_submit 内存分配失败: %s", e.what());
            // 尝试清理一些内存
            received_data.clear();
            received_data.shrink_to_fit();
        }
    }
    else
    {
        auto cmd_unlink_seqnum = std::get<1>(unlink_found);

        // 发送ret_unlink
        try
        {
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

    // 清理数据
    usb_host_transfer_free(trx);
    delete &callback_arg;
}