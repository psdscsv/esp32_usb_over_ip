#pragma once

#include <cstdint>
#include <array>
#include <atomic>
#include <shared_mutex>
#include <map>
#include <optional>
#include <memory>
#include <vector>
#include <usb/usb_host.h>

/**
 * @brief 无锁/分段锁的 USB 传输追踪数据结构
 *
 * 核心优化：
 * - 按端点分段：16个段对应256个USB端点地址，降低竞争
 * - 原子计数器：快速路径无需锁
 * - 延迟清理：周期性批量清理而非即时清理
 */
namespace usbipdcpp
{
    // 类型别名，将 usb_transfer_t 映射为 usb_transfer
    using usb_transfer = usb_transfer_t;

    class ConcurrentTransferTracker
    {
    public:
        static constexpr size_t SEGMENT_COUNT = 16; // 按端点分段数
        static constexpr size_t SEGMENT_SIZE = 16;  // 每段初始大小

        struct TransferInfo
        {
            std::uint32_t seqnum;
            usb_transfer *transfer;
            std::uint8_t endpoint;
            std::uint64_t submit_time; // 提交时间戳 (us)
        };

        ConcurrentTransferTracker();
        ~ConcurrentTransferTracker() = default;

        /**
         * @brief 注册一个新的转移追踪
         * @return true 成功, false 并发数超过限制
         */
        bool register_transfer(std::uint32_t seqnum, usb_transfer *transfer,
                               std::uint8_t endpoint);

        /**
         * @brief 查询转移是否存在
         */
        bool contains(std::uint32_t seqnum) const;

        /**
         * @brief 获取转移信息
         */
        std::optional<TransferInfo> get(std::uint32_t seqnum) const;

        /**
         * @brief 删除转移
         */
        bool remove(std::uint32_t seqnum);

        /**
         * @brief 删除特定端点的所有转移
         */
        size_t remove_endpoint(std::uint8_t endpoint);

        /**
         * @brief 获取当前并发数
         */
        size_t concurrent_count() const
        {
            return concurrent_transfer_count_.load(std::memory_order_acquire);
        }

        /**
         * @brief 获取最大并发限制
         */
        size_t max_concurrent() const
        {
            return max_concurrent_;
        }

        /**
         * @brief 设置最大并发限制
         */
        void set_max_concurrent(size_t max)
        {
            max_concurrent_ = max;
        }

        /**
         * @brief 检查是否可以分配N个并发槽位
         */
        bool can_allocate(size_t count) const
        {
            size_t current = concurrent_transfer_count_.load(std::memory_order_acquire);
            return current + count <= max_concurrent_;
        }

        /**
         * @brief 手动递增并发计数（用于跨越多个tracked transfers的操作）
         */
        void increment_count(size_t count)
        {
            concurrent_transfer_count_.fetch_add(count, std::memory_order_release);
        }

        /**
         * @brief 手动递减并发计数
         */
        void decrement_count(size_t count)
        {
            concurrent_transfer_count_.fetch_sub(count, std::memory_order_release);
        }

        /**
         * @brief 清空所有转移追踪
         */
        void clear();

        /**
         * @brief 获取超时的转移（用于清理）
         */
        std::vector<std::uint32_t> get_timed_out_transfers(
            std::uint64_t timeout_us, std::uint64_t now_us) const;

    private:
        /**
         * @brief 获取段索引
         */
        size_t get_segment_index(std::uint8_t endpoint) const
        {
            return (endpoint >> 4) & (SEGMENT_COUNT - 1);
        }

        mutable std::array<std::shared_mutex, SEGMENT_COUNT> segment_locks_;
        std::array<std::map<std::uint32_t, TransferInfo>, SEGMENT_COUNT> segments_;

        std::atomic<size_t> concurrent_transfer_count_{0};
        size_t max_concurrent_ = 32;
    };
}
