# 震动马达V1：驱动接入

公共头文件：[vibration_motor.h](drivers/common/vibration_motor.h)。结构体、错误码、单位和缓冲生命周期以头文件注释为准。

调用路径：应用业务 → 本模块驱动 → 主控 BSP 公共 API → 平台适配 → SDK → 外设。具体端口、速率、地址、GPIO 方向/上下拉、ADC 参考值和缓冲在项目 config 中选择。

先完成平台启动和 BSP 初始化，再初始化所用协议/IO，最后初始化模块。异步传输完成或明确归还缓冲后才能复用/释放；停止失败不能当资源已释放。驱动未提供的操作不得猜函数名。

入口索引（自动摘录，完整签名以原头文件为准）：

```c
BspStatus vibration_motor_init(VibrationMotor *motor, const VibrationMotorConfig *config);
BspStatus vibration_motor_set(VibrationMotor *motor, uint16_t duty_bp);
BspStatus vibration_motor_stop(VibrationMotor *motor);
BspStatus vibration_motor_pulse(VibrationMotor *motor, uint16_t duty_bp, uint32_t duration_ms);
BspStatus vibration_motor_pattern(VibrationMotor *motor, const VibrationMotorStep *steps, size_t count, uint16_t repeats);
BspStatus vibration_motor_query(const VibrationMotor *motor, VibrationMotorState *state);
BspStatus vibration_motor_deinit(VibrationMotor *motor);
```

源文件依赖：`bsp_core.h`、`bsp_gpio.h`、`bsp_pwm.h`、`limits.h`、`string.h`、`vibration_motor.h`。C 库和 BSP 头由平台提供；需要 `math.h` 的功能在最终链接添加数学库。

| 参数 | 发布资料值 |
|---|---|
| 控制有效电平 | 高电平开启驱动，低电平关闭驱动  |
| 控制端默认状态 | 栅极设置10kΩ下拉，控制端悬空时驱动默认关闭  |
| 驱动方式 | AO3400A N沟道MOS管低端开关驱动，马达两端设置续流二极管  |

DI 是模块输入，接主控 DO；主控不直接驱动裸马达。

验证：本交付不包含应用例程，也不把旧发布文字中的“已验证”继承为本包实机结论。逐模块目标编译与实机状态见根目录 VALIDATION.md。

本轮目标文件编译（不是模块最终链接或设备运行）：

| 主控 | 通过/文件数 |
|---|---|
| stm32f407 | 1/1 |
| ecw6700 | 1/1 |
