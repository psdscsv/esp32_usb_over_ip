#pragma once

#include <exception>
#include <spdlog/spdlog.h>

namespace usbipdcpp
{
    // 将协程未捕获的异常记录后吞掉，避免在asio线程中重新抛出导致程序终止
    inline void if_has_value_than_rethrow(std::exception_ptr e)
    {
        if (!e)
            return;
        try
        {
            std::rethrow_exception(e);
        }
        catch (const std::exception &ex)
        {
            SPDLOG_ERROR("未处理的协程异常: {}", ex.what());
        }
        catch (...)
        {
            SPDLOG_ERROR("未处理的协程异常: unknown");
        }
    }
}
