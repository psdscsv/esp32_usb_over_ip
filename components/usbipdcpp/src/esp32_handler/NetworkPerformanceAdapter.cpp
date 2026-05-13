#include "NetworkPerformanceAdapter.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <numeric>

namespace usbipdcpp
{

    NetworkPerformanceAdapter::NetworkPerformanceAdapter(size_t history_window)
        : history_window_(history_window),
          last_adjustment_(std::chrono::steady_clock::now()) {}

    void NetworkPerformanceAdapter::record_request_sent(uint32_t seqnum)
    {
        std::lock_guard lock(metrics_mutex_);
        pending_requests_[seqnum] = std::chrono::steady_clock::now();
    }

    void NetworkPerformanceAdapter::record_request_acked(uint32_t seqnum)
    {
        std::lock_guard lock(metrics_mutex_);

        auto it = pending_requests_.find(seqnum);
        if (it == pending_requests_.end())
        {
            return;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - it->second)
                           .count();

        rtt_history_.push_back(elapsed);
        if (rtt_history_.size() > history_window_)
        {
            rtt_history_.pop_front();
        }

        success_count_++;
        pending_requests_.erase(it);
    }

    void NetworkPerformanceAdapter::record_request_timeout(uint32_t seqnum)
    {
        std::lock_guard lock(metrics_mutex_);

        auto it = pending_requests_.find(seqnum);
        if (it != pending_requests_.end())
        {
            pending_requests_.erase(it);
        }

        timeout_count_++;
    }

    void NetworkPerformanceAdapter::update_throughput(size_t bytes, std::chrono::milliseconds elapsed)
    {
        if (elapsed.count() > 0)
        {
            double throughput_bps = (bytes * 8.0) / (elapsed.count() / 1000.0);
            double throughput_mbps = throughput_bps / (1024 * 1024);

            // 使用简单的移动平均
            double current = current_throughput_mbps_.load();
            double smoothed = (current * 0.7) + (throughput_mbps * 0.3);
            current_throughput_mbps_.store(smoothed);
        }
    }

    usbipdcpp::NetworkPerformanceAdapter::PerformanceMetrics NetworkPerformanceAdapter::get_current_metrics() const
    {
        std::shared_lock lock(metrics_mutex_);

        PerformanceMetrics metrics;
        metrics.rtt_ms = calculate_avg_rtt();
        metrics.throughput_mbps = current_throughput_mbps_.load();

        size_t total = success_count_.load() + timeout_count_.load();
        metrics.packet_loss_rate = total > 0 ? static_cast<double>(timeout_count_.load()) / total : 0.0;

        metrics.pending_requests = pending_requests_.size();
        metrics.timestamp = std::chrono::system_clock::now();

        return metrics;
    }

    uint32_t NetworkPerformanceAdapter::get_suggested_request_size() const
    {
        auto metrics = get_current_metrics();

        // 根据吞吐量和延迟调整USB请求大小
        // 基本策略：增加吞吐量的同时保持足够的并发度

        uint32_t base_size = 8192; // 8KB

        // 根据吞吐量缩放
        if (metrics.throughput_mbps > 100)
        {
            base_size = 65536; // 64KB
        }
        else if (metrics.throughput_mbps > 50)
        {
            base_size = 32768; // 32KB
        }
        else if (metrics.throughput_mbps > 10)
        {
            base_size = 16384; // 16KB
        }

        // 根据延迟微调：高延迟时增加块大小以容纳更多数据
        if (metrics.rtt_ms > 20)
        {
            uint32_t doubled = static_cast<uint32_t>(base_size) * 2;
            const uint32_t max_size = 131072u; // 最多128KB
            base_size = std::min(doubled, max_size);
        }

        return base_size;
    }

    void NetworkPerformanceAdapter::reset()
    {
        std::lock_guard lock(metrics_mutex_);

        pending_requests_.clear();
        rtt_history_.clear();
        success_count_.store(0);
        timeout_count_.store(0);
        current_throughput_mbps_.store(0);
    }

    double NetworkPerformanceAdapter::calculate_avg_rtt() const
    {
        if (rtt_history_.empty())
        {
            return 0;
        }

        double sum = std::accumulate(rtt_history_.begin(), rtt_history_.end(), 0.0);
        return sum / rtt_history_.size();
    }

} // namespace usbipdcpp
