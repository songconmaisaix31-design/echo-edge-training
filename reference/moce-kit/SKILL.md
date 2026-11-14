---
name: moce-contestant-kit
description: 使用本包 MOCE 主控、模块驱动和接线资料开发选手应用；按平台与模块索引查找编译和 API 依赖。
---

从 [README.md](README.md) 确定交付边界，再按 [MODULES.md](MODULES.md) 只加载所用模块；先看接线，再看 API。未知型号/方向/供电先查 [OPEN-ITEMS.md](OPEN-ITEMS.md)。

平台入口见 platforms/stm32f407/README.md 或 platforms/ecw6700/README.md。应用 → 驱动 → BSP → SDK → 硬件；Board 是固定能力描述，项目 config 显式选择 IO/ADC/协议参数，integration 负责启动/时钟/IRQ。先读随包 Board 和 BSP 声明，不采用原厂演示工程默认引脚。驱动与业务均通过公共 BSP 操作外设；配置层可使用当前平台公开配置类型。

此包不含传感器、屏幕、电机等应用例程。starter 只有平台基础入口和空 application_main。添加模块先确认资源、时钟、方向、缓冲所有权和停止语义，关闭不用的功能；不要把 DMA 与硬件定时器独立可用当成任意硬件自动路由。

先用实际 SDK 完整编译链接，再上板验证；[VALIDATION.md](VALIDATION.md) 区分本次证据与未验收项。缺文件/符号应查依赖，不能生成成功空实现。不得把别的芯片/Board 测试结果当成本组合验证。
