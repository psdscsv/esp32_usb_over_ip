#include "esp32_handler/ZeroCopyBuffer.h"
#include <spdlog/spdlog.h>

namespace usbipdcpp
{

    ZeroCopyBuffer::~ZeroCopyBuffer()
    {
        clear();
    }

    void ZeroCopyBuffer::add_fragment(const void *data, size_t length)
    {
        fragments_.emplace_back(data, length, nullptr);
    }

    void ZeroCopyBuffer::add_fragment(const void *data, size_t length,
                                      std::function<void()> cleanup)
    {
        fragments_.emplace_back(data, length, cleanup);
    }

    void ZeroCopyBuffer::add_shared_data(std::shared_ptr<std::vector<uint8_t>> data)
    {
        if (!data || data->empty())
            return;

        auto ptr = data->data();
        auto len = data->size();

        // 持有shared_ptr，确保数据在发送完成前不会被释放
        add_fragment(ptr, len, [data]() { /* data自动释放 */ });
    }

    std::vector<asio::const_buffer> ZeroCopyBuffer::get_buffers() const
    {
        std::vector<asio::const_buffer> result;
        result.reserve(fragments_.size());

        for (const auto &frag : fragments_)
        {
            result.emplace_back(frag.data, frag.length);
        }

        return result;
    }

    size_t ZeroCopyBuffer::total_bytes() const
    {
        size_t total = 0;
        for (const auto &frag : fragments_)
        {
            total += frag.length;
        }
        return total;
    }

    void ZeroCopyBuffer::clear()
    {
        for (auto &frag : fragments_)
        {
            if (frag.on_release)
            {
                try
                {
                    frag.on_release();
                }
                catch (...)
                {
                    SPDLOG_ERROR("Fragment cleanup exception caught");
                }
            }
        }
        fragments_.clear();
    }

    void ZeroCopyBuffer::release_callbacks()
    {
        for (auto &frag : fragments_)
        {
            frag.on_release = nullptr;
        }
    }

    // ============ FragmentedTransferContext ============

    FragmentedTransferContext::FragmentedTransferContext(
        uint32_t seqnum,
        size_t total_size,
        std::function<void(bool success)> completion_callback)
        : seqnum_(seqnum),
          total_size_(total_size),
          completion_callback_(completion_callback)
    {

        // 计算所需的分片数量
        // 这里假设通常使用64KB的分片
#ifdef CONFIG_USB_HOST_BULK_TRANSFER_MAX_SIZE
        constexpr size_t FRAGMENT_SIZE = CONFIG_USB_HOST_BULK_TRANSFER_MAX_SIZE;
#else
        constexpr size_t FRAGMENT_SIZE = 64 * 1024;
#endif

        size_t num_fragments = (total_size + FRAGMENT_SIZE - 1) / FRAGMENT_SIZE;
        fragment_status_.resize(num_fragments, false);
        fragment_data_.resize(num_fragments, {nullptr, 0});

        SPDLOG_DEBUG("创建分片传输上下文: seq={}, 总大小={}, 分片数={}",
                     seqnum, total_size, num_fragments);
    }

    void FragmentedTransferContext::mark_fragment_done(size_t fragment_idx, bool success)
    {
        std::lock_guard lock(lock_);

        if (fragment_idx >= fragment_status_.size())
        {
            SPDLOG_WARN("无效的分片索引: {} >= {}", fragment_idx, fragment_status_.size());
            return;
        }

        if (!fragment_status_[fragment_idx])
        {
            fragment_status_[fragment_idx] = true;
            completed_count_++;

            if (!success)
            {
                SPDLOG_WARN("分片 {} 传输失败", fragment_idx);
            }
        }

        // 检查所有分片是否完成
        if (completed_count_ == fragment_status_.size() && completion_callback_)
        {
            bool all_success = std::all_of(
                fragment_status_.begin(),
                fragment_status_.end(),
                [](bool status)
                { return status; });

            try
            {
                completion_callback_(all_success);
            }
            catch (...)
            {
                SPDLOG_ERROR("完成回调异常");
            }
        }
    }

    bool FragmentedTransferContext::is_complete() const
    {
        std::lock_guard lock(lock_);
        return completed_count_ == fragment_status_.size();
    }

    void FragmentedTransferContext::register_fragment(size_t idx, const void *data, size_t length)
    {
        std::lock_guard lock(lock_);

        if (idx >= fragment_data_.size())
        {
            SPDLOG_WARN("无效的分片索引进行注册: {} >= {}", idx, fragment_data_.size());
            return;
        }

        fragment_data_[idx] = {data, length};
    }

    ZeroCopyBuffer FragmentedTransferContext::build_zero_copy_buffer() const
    {
        std::lock_guard lock(lock_);

        ZeroCopyBuffer buffer;
        for (const auto &[data, length] : fragment_data_)
        {
            if (data && length > 0)
            {
                buffer.add_fragment(data, length);
            }
        }

        return buffer;
    }

} // namespace usbipdcpp
