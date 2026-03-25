#include "ConcurrentTransferTracker.h"
#include <usb/usb_host.h>
#include <spdlog/spdlog.h>
#include <esp_timer.h>

namespace usbipdcpp
{

    ConcurrentTransferTracker::ConcurrentTransferTracker()
        : segment_locks_(), segments_()
    {
        SPDLOG_INFO("初始化并发转移追踪器，分段数: {}, 最大并发: {}",
                    SEGMENT_COUNT, max_concurrent_);
    }

    bool ConcurrentTransferTracker::register_transfer(
        std::uint32_t seqnum, usb_transfer *transfer, std::uint8_t endpoint)
    {

        // 快速路径：检查是否超过限制（无锁）
        size_t current_count = concurrent_transfer_count_.load(std::memory_order_acquire);
        if (current_count >= max_concurrent_)
        {
            SPDLOG_WARN("并发转移数超过限制: {} >= {}", current_count, max_concurrent_);
            return false;
        }

        // 获取分段锁
        size_t segment_idx = get_segment_index(endpoint);
        std::lock_guard lock(segment_locks_[segment_idx]);

        // 重新检查（再次确认）
        current_count = concurrent_transfer_count_.load(std::memory_order_acquire);
        if (current_count >= max_concurrent_)
        {
            return false;
        }

        // 注册转移
        auto [it, inserted] = segments_[segment_idx].emplace(
            seqnum,
            TransferInfo{
                .seqnum = seqnum,
                .transfer = transfer,
                .endpoint = endpoint,
                .submit_time = static_cast<std::uint64_t>(esp_timer_get_time())});

        if (inserted)
        {
            concurrent_transfer_count_.fetch_add(1, std::memory_order_release);
            return true;
        }

        return false;
    }

    bool ConcurrentTransferTracker::contains(std::uint32_t seqnum) const
    {
        // 快速搜索：遍历所有段（最多16个）
        for (size_t i = 0; i < SEGMENT_COUNT; ++i)
        {
            std::shared_lock lock(segment_locks_[i]);
            if (segments_[i].count(seqnum) > 0)
            {
                return true;
            }
        }
        return false;
    }

    std::optional<ConcurrentTransferTracker::TransferInfo>
    ConcurrentTransferTracker::get(std::uint32_t seqnum) const
    {
        for (size_t i = 0; i < SEGMENT_COUNT; ++i)
        {
            std::shared_lock lock(segment_locks_[i]);
            auto it = segments_[i].find(seqnum);
            if (it != segments_[i].end())
            {
                return it->second;
            }
        }
        return std::nullopt;
    }

    bool ConcurrentTransferTracker::remove(std::uint32_t seqnum)
    {
        for (size_t i = 0; i < SEGMENT_COUNT; ++i)
        {
            std::lock_guard lock(segment_locks_[i]);
            auto it = segments_[i].find(seqnum);
            if (it != segments_[i].end())
            {
                segments_[i].erase(it);
                concurrent_transfer_count_.fetch_sub(1, std::memory_order_release);
                return true;
            }
        }
        return false;
    }

    size_t ConcurrentTransferTracker::remove_endpoint(std::uint8_t endpoint)
    {
        size_t segment_idx = get_segment_index(endpoint);
        std::lock_guard lock(segment_locks_[segment_idx]);

        size_t removed = 0;
        auto &segment = segments_[segment_idx];

        for (auto it = segment.begin(); it != segment.end();)
        {
            if (it->second.endpoint == endpoint)
            {
                it = segment.erase(it);
                removed++;
            }
            else
            {
                ++it;
            }
        }

        if (removed > 0)
        {
            concurrent_transfer_count_.fetch_sub(removed, std::memory_order_release);
            SPDLOG_DEBUG("移除端点 {:02x} 的 {} 个转移", endpoint, removed);
        }

        return removed;
    }

    void ConcurrentTransferTracker::clear()
    {
        // 逐个获取锁并清空
        size_t total_removed = 0;
        for (size_t i = 0; i < SEGMENT_COUNT; ++i)
        {
            std::lock_guard lock(segment_locks_[i]);
            total_removed += segments_[i].size();
            segments_[i].clear();
        }

        concurrent_transfer_count_.store(0, std::memory_order_release);
        SPDLOG_INFO("清空所有 {} 个转移", total_removed);
    }

    std::vector<std::uint32_t> ConcurrentTransferTracker::get_timed_out_transfers(
        std::uint64_t timeout_us, std::uint64_t now_us) const
    {

        std::vector<std::uint32_t> timed_out;

        for (size_t i = 0; i < SEGMENT_COUNT; ++i)
        {
            std::shared_lock lock(segment_locks_[i]);
            for (const auto &[seqnum, info] : segments_[i])
            {
                if (now_us - info.submit_time > timeout_us)
                {
                    timed_out.push_back(seqnum);
                }
            }
        }

        if (!timed_out.empty())
        {
            SPDLOG_WARN("检测到 {} 个超时的转移", timed_out.size());
        }

        return timed_out;
    }

} // namespace usbipdcpp
