ESP32 USB/IP Device 增强版
https://img.shields.io/badge/License-Apache%25202.0-blue.svg
https://img.shields.io/badge/%E5%9F%BA%E4%BA%8E-yunsmall/usbipdcpp__esp32-green.svg
https://img.shields.io/badge/%E5%B9%B3%E5%8F%B0-ESP32-orange.svg

一个基于开源项目 usbipdcpp_esp32 的增强版本，专注于优化和稳定 USB/IP 协议在 ESP32 上的数据传输。

本项目在原作基础上，对 USB/IP 协议栈和数据传输路径进行了重构与优化，显著提升了设备兼容性、传输速度和连接稳定性，旨在让 ESP32 成为一个更可靠的远程 USB 设备解决方案。

与原项目相比，本分支主要实现了以下改进：

🚀 增强的数据传输核心：重构了数据包处理逻辑，优化了缓冲区管理，减少了传输延迟并提高了吞吐量。

🔧 提升设备兼容性：修复了特定 USB 设备（如复合设备、某些 HID 设备）的枚举与通信问题。

⚡ 改善连接稳定性：增强了异常处理和重连机制，使长时间运行更加稳定可靠。

📄 更详细的日志与调试：增加了更多运行时日志级别，便于问题诊断和开发调试。

🛠️ 简化的构建与配置：改进了项目结构，使编译和配置选项更加清晰直观。

📝 许可证与致谢

许可证
本项目基于 Apache License Version 2.0 开源。

原始代码版权归属于原项目作者 yunsmall。

所有修改和新增的代码同样遵循 Apache 2.0 许可证。

在分发或使用本作品时，必须保留原作者的版权、专利、商标及 attribution 声明。具体细节请查阅完整的 LICENSE 文件。
