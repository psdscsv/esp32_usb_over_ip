#pragma once

#include <memory>
#include <spdlog/spdlog.h>
#include <usb/usb_host.h> // 包含 usb_transfer_t 的定义

struct UsbTransferDeleter
{
    void operator()(usb_transfer_t *t) const noexcept
    {
        if (t)
        {
            usb_host_transfer_free(t);
        }
    }
};

using UsbTransferPtr = std::unique_ptr<usb_transfer_t, UsbTransferDeleter>;