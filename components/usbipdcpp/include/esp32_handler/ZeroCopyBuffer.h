#pragma once

#include <vector>
#include <cstdint>
#include <memory>
#include <asio.hpp>

/**
 * @brief 零拷贝发送缓冲区管理器
 *
 * 支持分散-聚集（scatter-gather）模式，避免大数据拷贝
 * 主要用于处理大型USB传输的分片，保持零拷贝特性
 */
namespace usbipdcpp
{

    class ZeroCopyBuffer
    {
    public:
        /**
         * @brief 缓冲区片段，指向一段内存但不持有所有权
         */
        struct Fragment
        {
            const void *data;
            size_t length;

            // 可选的所有权管理器，用于fragment生命周期结束后清理
            std::function<void()> on_release;

            explicit Fragment(const void *d, size_t len,
                              std::function<void()> cleanup = nullptr)
                : data(d), length(len), on_release(cleanup) {}
        };

        ZeroCopyBuffer() = default;
        ~ZeroCopyBuffer();

        /**
         * @brief 添加一个内存片段（不拷贝）
         */
        void add_fragment(const void *data, size_t length);

        /**
         * @brief 添加一个内存片段，带自动清理函数
         */
        void add_fragment(const void *data, size_t length,
                          std::function<void()> cleanup);

        /**
         * @brief 添加一个shared_ptr持有的字节向量
         */
        void add_shared_data(std::shared_ptr<std::vector<uint8_t>> data);

        /**
         * @brief 获取所有片段用于asio缓冲
         */
        std::vector<asio::const_buffer> get_buffers() const;

        /**
         * @brief 获取总数据字节数
         */
        size_t total_bytes() const;

        /**
         * @brief 清空所有片段
         */
        void clear();

        /**
         * @brief 移除cleanup的callback（用于所有权转移后）
         */
        void release_callbacks();

    private:
        std::vector<Fragment> fragments_;
    };

    /**
     * @brief 分片上下文，用于跟踪大型传输的分片
     *
     * 避免在callback中使用锁，完全基于引用计数
     */
    class FragmentedTransferContext : public std::enable_shared_from_this<FragmentedTransferContext>
    {
    public:
        FragmentedTransferContext(
            uint32_t seqnum,
            size_t total_size,
            std::function<void(bool success)> completion_callback);

        /**
         * @brief 标记一个分片已完成
         */
        void mark_fragment_done(size_t fragment_idx, bool success);

        /**
         * @brief 获取是否全部完成
         */
        bool is_complete() const;

        /**
         * @brief 注册缓冲区片段
         */
        void register_fragment(size_t idx, const void *data, size_t length);

        /**
         * @brief 构建用于发送的零拷贝缓冲
         */
        ZeroCopyBuffer build_zero_copy_buffer() const;

        uint32_t get_seqnum() const { return seqnum_; }
        size_t get_total_size() const { return total_size_; }

    private:
        uint32_t seqnum_;
        size_t total_size_;
        std::vector<bool> fragment_status_;
        std::vector<std::pair<const void *, size_t>> fragment_data_;
        size_t completed_count_ = 0;
        std::function<void(bool)> completion_callback_;
        mutable std::recursive_mutex lock_;
    };

}
