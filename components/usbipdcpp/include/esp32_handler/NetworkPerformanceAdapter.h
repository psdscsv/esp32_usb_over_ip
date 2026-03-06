#pragma once

#include <cstdint>
#include <chrono>
#include <atomic>
#include <deque>
#include <shared_mutex>
#include <map>

/**
 * @brief 自适应网络性能监测与动态调优
 *
 * 持续监测网络往返延迟（RTT）、带宽利用率、丢包率等指标，
 * 并根据这些指标动态调整批处理大小，优化吞吐量
 */
namespace usbipdcpp
{

    class NetworkPerformanceAdapter
    {
    public:
        /**
         * @brief 网络性能指标
         */
        struct PerformanceMetrics
        {
            double rtt_ms = 0;           // 往返延迟（毫秒）
            double throughput_mbps = 0;  // 吞吐量（Mbps）
            double packet_loss_rate = 0; // 丢包率 (0.0-1.0)
            size_t pending_requests = 0; // 待处理请求数
            std::chrono::system_clock::time_point timestamp;
        };

        /**
         * @brief 动态批处理配置建议
         */
        struct BatchConfigSuggestion
        {
            size_t max_batch_size = 32;
            size_t max_batch_bytes = 65536;
            std::chrono::milliseconds max_batch_delay{5};
        };

        NetworkPerformanceAdapter(size_t history_window = 100);

        /**
         * @brief 记录请求的发送时间戳
         */
        void record_request_sent(uint32_t seqnum);

        /**
         * @brief 记录请求的确认信息
         */
        void record_request_acked(uint32_t seqnum);

        /**
         * @brief 记录请求的超时
         */
        void record_request_timeout(uint32_t seqnum);

        /**
         * @brief 更新当前网络吞吐量（基于传输的字节数）
         */
        void update_throughput(size_t bytes, std::chrono::milliseconds elapsed);

        /**
         * @brief 获取当前性能指标
         */
        PerformanceMetrics get_current_metrics() const;

        /**
         * @brief 根据当前网络状况获取最优的批处理建议
         */
        BatchConfigSuggestion get_batch_config_suggestion() const;

        /**
         * @brief 获取建议的USB请求大小
         * 基于当前网络带宽和延迟
         */
        uint32_t get_suggested_request_size() const;

        /**
         * @brief 重置统计信息
         */
        void reset();

    private:
        /**
         * @brief 计算平均往返延迟
         */
        double calculate_avg_rtt() const;

        std::map<uint32_t, std::chrono::steady_clock::time_point> pending_requests_;
        std::deque<double> rtt_history_;
        size_t history_window_;

        std::atomic<size_t> success_count_{0};
        std::atomic<size_t> timeout_count_{0};
        std::atomic<double> current_throughput_mbps_{0};

        mutable std::shared_mutex metrics_mutex_;

        std::chrono::steady_clock::time_point last_adjustment_;
        static constexpr auto ADJUSTMENT_INTERVAL = std::chrono::seconds(5);
    };

}
