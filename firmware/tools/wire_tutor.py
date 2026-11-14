#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
接线陪练 —— 实时显示板子报回来的中文提示。

用法：
    python wire_tutor.py                # 自动找板子并监听
    python wire_tutor.py --port COM5    # 手动指定端口
    python wire_tutor.py --reset        # 监听时顺便复位一次板子

按 Ctrl+C 退出。
"""
from __future__ import annotations

import argparse
import sys
import time

try:
    import serial
    import serial.tools.list_ports as list_ports
except ImportError:
    sys.exit("缺少 pyserial，请先执行: python -m pip install pyserial")

BAUD = 115200


def find_port() -> str | None:
    """优先找 ESP32-S3 原生 USB 口，其次找 CH343 转串口。"""
    ports = list(list_ports.comports())
    for vid, pid_hint in ((0x303A, "原生 USB 口"), (0x1A86, "CH343 转串口")):
        for p in ports:
            if p.vid == vid:
                print(f"找到板子：{p.device}（{pid_hint}）")
                return p.device
    if ports:
        print("没认出板子，但发现了这些串口：", ", ".join(p.device for p in ports))
        return ports[0].device
    return None


def explain(line: str) -> str | None:
    """给关键事件补一句人话解释。"""
    if "【接上了】" in line:
        return "  → 接对了！这根线通了，可以接着往下接。"
    if "【断开了】" in line:
        return "  → 有根线松了或接触不良，断电后重新插紧那根杜邦线。"
    if "【等待中】" in line:
        return "  → 现在还没接模块，属于正常状态，不用紧张。"
    if "接线助手 已启动" in line:
        return "  → 板子已经在帮你看着总线了，可以开始接线。"
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description="接线陪练")
    ap.add_argument("--port", help="手动指定串口，如 COM5")
    ap.add_argument("--reset", action="store_true", help="打开后复位一次板子")
    args = ap.parse_args()

    port = args.port or find_port()
    if not port:
        print()
        print("没找到板子。请检查：")
        print("  1) USB 线插好了吗？（要能传数据的线，不能是纯充电线）")
        print("  2) 板子上有没有灯亮？")
        print("  3) 换个 USB 口或换根线再试")
        return 1

    print()
    print("=" * 58)
    print("  接线陪练已连接。现在可以开始接线了。")
    print("  提醒：插拔杜邦线之前，先拔掉 USB 线断电。")
    print("=" * 58)
    print()

    try:
        ser = serial.Serial(port, BAUD, timeout=0.05)
    except Exception as exc:  # noqa: BLE001
        print(f"串口打不开：{exc}")
        print("如果串口监视器或别的软件占着这个口，先把它关掉。")
        return 1

    if args.reset:
        ser.setDTR(False)
        ser.setRTS(True)
        time.sleep(0.15)
        ser.setRTS(False)

    buf = b""
    last_data = time.monotonic()
    warned = False
    try:
        while True:
            if ser.in_waiting:
                buf += ser.read(ser.in_waiting)
                last_data = time.monotonic()
                warned = False
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    line = raw.decode("utf-8", "replace").rstrip("\r")
                    if not line.strip():
                        continue
                    stamp = time.strftime("%H:%M:%S")
                    print(f"[{stamp}] {line}")
                    note = explain(line)
                    if note:
                        print(note)
                    sys.stdout.flush()
            else:
                time.sleep(0.02)

            if not warned and time.monotonic() - last_data > 8:
                print()
                print("（8 秒没收到板子的消息了。可能是：USB 线松了 / 板子没供电 /")
                print("  串口选错了。检查后按 Ctrl+C 退出再重新运行本程序。）")
                warned = True
    except KeyboardInterrupt:
        print("\n\n陪练已退出。板子会继续自己扫描，随时可以再连上来。")
    finally:
        ser.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
