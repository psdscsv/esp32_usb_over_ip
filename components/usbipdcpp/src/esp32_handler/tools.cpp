#include "tools.h"

using namespace usbipdcpp;

UsbSpeed usbipdcpp::esp32_speed_to_usb_speed(int speed)
{
    switch (speed)
    {
    case USB_SPEED_LOW:
        return UsbSpeed::Low;
    case USB_SPEED_FULL:
        return UsbSpeed::Full;
    case USB_SPEED_HIGH:
        return UsbSpeed::High;
    default:
        ESP_LOGD("usbipdcpp_tools", "unknown speed enum %d", speed);
        return UsbSpeed::Unknown;
    }
}
