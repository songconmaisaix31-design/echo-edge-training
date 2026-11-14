/*
# 灯板驱动的适用范围与集成要求

本说明由维护者按 2026-11-12 用户授权人工维护，依据已发布 ws2812b_do_dma.c/h 和 STM32F407 BSP 源码整理；未进行实物测试。

## 能力与适用范围

本驱动控制 WS2812B-Mini-V6 64 像素灯板，通过公共 bsp_timer_dma_gpio_start/query/stop 接口工作。需要真实的定时器触发 DMA 向普通 GPIO 的置位/复位寄存器输出 32 位序列能力；普通 GPIO blocking 能力或头文件存在不代表满足该要求。驱动保持通用 BSP 调用，不直接调用厂商 HAL。

本次已核对的主控为 STM32F407VET6，配套 STM32F407-BSP-1.0.0-alpha.1-audio-20261104-1。后台平台标签 stm32 仅表示平台分类，不声明所有 STM32 型号都可用；其它型号必须核对同等真实能力后再维护适用范围。本版本不声明其它平台支持。

## F407 集成清单

- 在所有相关编译单元一致定义 BSP_F407_TIMER_DMA_GPIO=1，使用真实 GPIO DMA 分支；未启用时对应接口返回 BSP_UNSUPPORTED。
- 本次语法检查配套启用 BSP_F407_DMA_EXTENSIONS=1、BSP_F407_SYSTEM_EXTENSIONS=1；保留所选 BSP 对其它功能的配置要求，不把示例选项当成所有工程必须全部启用。
- Board 必须包含 BOARD_TIMER_GPIO_DMA 对应 TIM8 / TIM8_UP_TIM13_IRQn。真实路由为 TIM8_UP -> DMA2 Stream1 Channel7 -> 目标 GPIO BSRR；检查与其它端口、定时器及 DMA 的资源冲突，链接配套 bsp_timer.c（含 bsp_timer_dma_gpio.inc）、bsp_dma_native.c 及真实公共依赖，保留 DMA2_Stream1 的正确中断处理。
- 例程的 board_clock.h / board_clock_init 属于工程启动集成，应提供匹配实际晶振的实现。示例 tick_hz=168000000、period_ticks=54 要求实际 APB2 定时器时钟匹配；不得只填频率常数而未配置或验证时钟。
- GPIO 应先经 Board 组合为普通推挽输出、初始低电平，再初始化端口与驱动。pin_mask 必须来自所选 Board 中实际连接灯板 DI 的引脚。例程 DO0/PD2 是特定组合示例，具体项目从已冻结拓扑与主控/转接板 Board 解析，不能把该引脚当作所有项目固定值。
- words 需要 8192 个对齐 uint32_t（32768 字节），放在 DMA 可访问的普通 SRAM，不能放入 CCM；实例与缓冲在 DMA 借用期间持续存活，不得编辑，stop 成功后才可复用。
- 此实现为循环整帧输出；修改图案先 stop，再编辑/start。stop 不自动发送黑帧。复位低电平、波形时序和实物像素顺序仍需实机确认；蛇形、行列及翻转参数由项目实际灯板布局决定。

缺少任何真实底层能力时应明确返回不支持，不能用空实现或将编译通过当成灯板可用。此维护只明确适用条件，不修改既有 C/H 算法、不修复历史生成代码，也不提高实物验证等级。

*/
