# 接线总览

按 [模块索引](MODULES.md) 进入每个模块的 WIRING.md，逐针信号表都在同一个位置维护。主控侧见 [F407端口映射](platforms/stm32f407/PORT-MAP.md) / [ECW端口映射](platforms/ecw6700/PORT-MAP.md)。

表中针号为图纸触点号，不是从任意观察面数的位置。上电前确认实物pin1/防呆方向、端子端是否镜像、模块供电及信号电平；UART的TX/RX需按方向交叉。不得把未连接NC当成电源，麦克风、功放、灯板均需特别看具体线序。未知项见 [OPEN-ITEMS.md](OPEN-ITEMS.md)。
