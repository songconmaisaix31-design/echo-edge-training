# ECW6700F 主控（2ADC）

BSP API 1.0.0-alpha.1；适配修订 audiofix.20261112.1；SDK 固定 ECW6700FSDK V1.0.0B02P12。Board 是 schematic-20261009-2adc，SHEILD=none。其它硬件版本需选匹配 Board，不能改 SDK 演示默认引脚替代。

## 固定依赖与准备

Python **3.11 或以上**。原厂 P12 和 Linux 编译器的 URL、SHA-256、大小在 [downloads.json](downloads.json)；原包SDK298906827字节，SHA256 084618cdb6ab6f9c014e683fafff4ae82c3b77d3f424018a8d0e5604aa1c229c。Linux 编译器固定 riscv-ubuntu20_04-elf-2025_0917_0905。Windows 使用本次单独提供的 tools-only 附件（compiler/make/git/flash）；身份见 tool-downloads.json。不能把 Ubuntu 编译器当成 Windows exe。

**原厂 P12 不能直接替代准备后的 SDK。** 依赖随包 sdk-extension 的21文件匹配集合，含 I2S/DMA、ADC与构建对象隔离等；不是仅重命名函数。本包保留 prepare_sdk 和逐文件 before/after身份。不要手改安装SDK，也不要把某个私有文件复制进驱动层。

在本目录执行，output 必须是不存在的新目录：

```powershell
python -B sdk-extension/prepare_sdk.py --archive "<download>/ECW6700FSDK_V1.0.0B02P12.tar.gz" --output "<new-sdk-directory>"
python -B prepare_project.py --sdk "<new-sdk-directory>/ECW6700FSDK_V1.0.0B02P12/code" --name moce_contestant
python -B build_project.py --sdk "<new-sdk-directory>/ECW6700FSDK_V1.0.0B02P12/code" --name moce_contestant --windows-tools "<tools-root>"
```

Linux 使用同一准备流程，将固定ECW编译器bin放PATH，build_project不传windows-tools。完整SDK构建路径须避免空格；新项目拷入 Boards/ecw6700f/<name>，SDK其余源码不由本脚本修改。不要在已配置/构建过的 SDK 中混用另一份BSP或应用对象。首次构建应在新准备目录执行。

## 初始化与应用

调用：SDK启动 → starter/startup.c main → 原厂 Flash/RF/ROM/BLE/WiFi系统初始化 → FreeRTOS调度 → project_run → config.c IO空合成 → bsp_platform_init校验运行上下文/时钟 → 空 application_main。

使用 SDK CPU160/AHB80/APB40、500Hz Tick；这些启动依赖没有从工程中消失。原厂系统仍包含无线库，不代表空应用运行了BSP无线worker；本工程BSP RADIO/I2S/DMA/SYSTEM默认关闭，需使用时显式打开并补全各自参数/生命周期。启动任务配置4096字节为本工程选择，尚未实测栈峰值。

业务写 application.c；配置改 config.c/config.h。新增驱动源须加入 SDK app 的 CSRCS/VPATH/INCPATHS；不要只拷头文件。BSP源列表由 bsp/bsp_sources.mk提供，Board board.c只编译一次。驱动不得直接调用SDK。平台公开配置见 [bsp_ecw6700.h](bsp/include/bsp_ecw6700.h)、[IO合成](bsp/include/bsp_ecw6700_io_config.h)。启用音频/显式DMA须检查依赖和实际硬件路由，不能用成功空实现补缺失符号。

## 烧录与输出

输出位于SDK code/build/ecw6700f/<name>/，包括 ELF、应用镜像及SDK配套镜像/布局；不要把应用 ELF 当成完整Flash映像。配套烧录器身份与使用方式按本包tool-downloads及独立工具附件MANIFEST核对。本轮未连接板卡，不代执行下载；仅凭COM口存在不能确定哪块板。

接线以本包board/为芯片映射依据；模块针序见各WIRING.md。下载串口与console可能共享原厂UART1，不能再由普通UART实例重复抢占；业务UART须按Board实际引出和占用选择。启动成功后在调试器观察 project_startup_status，UART无日志不等于BSP失败。

[Board 到芯片引脚表](PORT-MAP.md) 供接线检索；实际冲突和资源资格仍以随包 Board 为准。

工具附件：[文件名/大小/SHA256](tool-downloads.json)。由主办方与本资料包分别发放；解压根目录含compiler/、make/、git/、flash/，不含SDK/BSP/例程或内部仓库资料。SDK按本页原厂URL另行下载和应用扩展。实际编译器ESWIN GCC14.1.1（2025-09-17）、GNU Make4.2.1。本轮新基础工程验证宿主为Windows；Linux构建入口提供但未在本轮执行，macOS没有本套新工程验收证据。

配套烧录器是 flash/eswin_tool.exe 2.1.13（历史版本与哈希已登记，本轮仅交付原字节，未执行烧录）。用 version 和 flash -h 检查工具；实际写入需由操作者确认串口和本工程 allinone 镜像后执行 flash --device <confirmed-port> --allinoneFile <confirmed-allinone>。不能猜偏移、用SDK演示镜像或把CLI写入成功等同于设备功能正常。

配套烧录器是 flash/eswin_tool.exe 2.1.13（历史版本与哈希已登记，本轮仅交付原字节，未执行烧录）。用 version 和 flash -h 检查工具；实际写入需由操作者确认串口和本工程 allinone 镜像后执行 flash --device <confirmed-port> --allinoneFile <confirmed-allinone>。不能猜偏移、用SDK演示镜像或把CLI写入成功等同于设备功能正常。
