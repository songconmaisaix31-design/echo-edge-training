---
name: moce-mg90s-servo
description: 使用 MOCE MG90S舵机V1 硬件及随包驱动开发；读取适用版本、接线和公共 API。
---

先读 [WIRING.md](WIRING.md)，再读 [API.md](API.md) 和链接的驱动头。确认所选主控 BSP 能力、资源冲突、供电和时钟；未知线序或型号冲突先向供货方确认，不套用同插头模块。

按 应用 → 驱动 → BSP → SDK 的路径开发；config 负责端口/速率/IO选择，平台 integration 负责启动和 IRQ。不要让模块驱动直接调用 HAL/SDK。本包没有可复制的应用例程；需要自己实现业务与验收。编译、链接、上板运行分别记录。
