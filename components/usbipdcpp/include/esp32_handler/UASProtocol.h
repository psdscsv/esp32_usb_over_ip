#pragma once

#include <cstdint>
#include <vector>
#include <array>
#include <optional>
#include <memory>

/**
 * @brief USB Attached SCSI (UAS) 协议实现框架
 *
 * 提供UAS IU (Information Unit) 的基本定义和处理
 * 当设备支持UAS时，可用此协议替代Bulk-Only
 */
namespace usbipdcpp::uas
{

    // ============ UAS 常量 ============

    constexpr uint8_t USB_PIPE_COMMAND = 0x01;  // Command IU pipe
    constexpr uint8_t USB_PIPE_STATUS = 0x02;   // Status IU pipe
    constexpr uint8_t USB_PIPE_DATA_IN = 0x03;  // Data IN pipe
    constexpr uint8_t USB_PIPE_DATA_OUT = 0x04; // Data OUT pipe

    // UAS 指令类型
    enum class CommandIUType : uint8_t
    {
        COMMAND = 0x00,
        TASK_MGMT = 0x01,
        READ_READY = 0x02,
        WRITE_READY = 0x03
    };

    // ============ UAS Information Unit 结构 ============

    /**
     * @brief Command Information Unit (Command IU)
     * 用于向设备发送SCSI命令
     */
    struct CommandIU
    {
        static constexpr size_t IU_ID = 0x00;

        uint8_t iu_id = IU_ID;
        uint8_t reserved1 = 0;
        uint16_t tag; // 命令标签，用于追踪（1-254）
        uint8_t reserved2 = 0;
        uint8_t prio_attr; // Priority/Attributes
        uint8_t reserved3 = 0;
        uint8_t len;     // CDB长度
        uint8_t cdb[16]; // SCSI CDB
        // ..可扩展为32字节CDB...

        std::vector<uint8_t> to_bytes() const;
    };

    /**
     * @brief Task Management Information Unit
     * 用于任务管理（ABORT, LUN RESET等）
     */
    struct TaskMgmtIU
    {
        static constexpr size_t IU_ID = 0x01;

        uint8_t iu_id = IU_ID;
        uint8_t reserved1 = 0;
        uint16_t tag;
        uint8_t reserved2[3] = {};
        uint8_t function; // Task management function
        uint8_t reserved3[8] = {};

        enum Function : uint8_t
        {
            ABORT_TASK = 0x01,
            ABORT_TASK_SET = 0x02,
            CLEAR_TASK_SET = 0x04,
            LOGICAL_UNIT_RESET = 0x08,
            I_T_NEXUS_RESET = 0x10,
            CLEAR_ACA = 0x40,
            QUERY_TASK = 0x80
        };

        std::vector<uint8_t> to_bytes() const;
    };

    /**
     * @brief Status Information Unit
     * 设备返回的状态信息
     */
    struct StatusIU
    {
        static constexpr size_t IU_ID = 0x43; // 'C'

        uint8_t iu_id = IU_ID;
        uint8_t reserved1 = 0;
        uint16_t tag;
        uint32_t residual;      // 剩余字节数
        uint8_t status;         // SCSI status
        uint8_t sense_len;      // 感知数据长度
        uint16_t sense_iu_len;  // 感知IU长度
        uint8_t sense_data[18]; // 感知信息

        static StatusIU from_bytes(const std::vector<uint8_t> &data);
        bool is_success() const { return status == 0x00; }
    };

    /**
     * @brief Ready to Transfer Information Unit
     * 设备准备进行数据传输
     */
    struct ReadyToTransferIU
    {
        static constexpr size_t IU_ID = 0x39; // '9'

        uint8_t iu_id = IU_ID;
        uint8_t reserved1 = 0;
        uint16_t tag;
        uint32_t requested_offset; // 请求的偏移
        uint32_t requested_length; // 请求的长度
        uint8_t reserved2[4] = {};
    };

    // ============ UAS 命令追踪器 ============

    /**
     * @brief 管理并发的UAS命令
     */
    class CommandTracker
    {
    public:
        static constexpr size_t MAX_COMMANDS = 254;

        CommandTracker();

        /**
         * @brief 分配一个新的命令标签
         */
        std::optional<uint16_t> allocate_tag();

        /**
         * @brief 释放命令标签
         */
        void release_tag(uint16_t tag);

        /**
         * @brief 获取可用标签数量
         */
        size_t available_tags() const;

        /**
         * @brief 检查标签是否被使用
         */
        bool is_tag_used(uint16_t tag) const;

    private:
        std::array<bool, MAX_COMMANDS> tag_used_{};
        size_t next_tag_ = 1;
    };

    // ============ UAS 协议处理器 ============

    /**
     * @brief UAS协议支持检测与协商
     */
    class UASNegotiator
    {
    public:
        /**
         * @brief 检查设备是否支持UAS
         * @return 支持则返回true
         */
        static bool probe_device_support(void *device_handle);

        /**
         * @brief 启用UAS协议
         */
        static bool enable_uas(void *device_handle);

        /**
         * @brief 获取设备支持的管道数
         */
        static struct
        {
            uint8_t command_pipe;
            uint8_t status_pipe;
            uint8_t data_in_pipe;
            uint8_t data_out_pipe;
        } get_device_pipes(void *device_handle);
    };

} // namespace usbipdcpp::uas
