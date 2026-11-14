#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
ecw_bridge_cli.py - 通过 ESP32 的 EcwBridge 固件, 把命令转发给 ECW6700 的 CLI

原理: 先让 ESP32 进入透传模式(BRIDGE tx rx baud), 之后 COM4 就等同于 ECW 的串口。
      本脚本负责: 进桥 -> 逐条发命令 -> 收结果 -> 退出桥。

用法:
    python ecw_bridge_cli.py help
    python ecw_bridge_cli.py get_mac get_sn get_project_id part
    python ecw_bridge_cli.py --raw help          # 保留 [时:分:秒.毫秒][cmd-I][1] 前缀
    python ecw_bridge_cli.py --tx 18 --rx 17 xxx # TX/RX 接反时用这个

注意:
    - 命令用 \\r 结尾(这个 CLI 不认 \\n, 实测 \\r\\n 反而无响应)
    - 透传模式按 ESC(0x1B) 退出, 所以命令里不能出现 0x1B
"""
import argparse
import re
import sys
import time

import serial

# [00:23:48.748][cmd-I][1] 这种前缀
PREFIX = re.compile(r"\[\d{2}:\d{2}:\d{2}\.\d+\]\[[^\]]*\]\[\d+\]")


def strip_prefix(text):
    out = []
    for line in text.splitlines():
        line = PREFIX.sub("", line)
        if line.strip():
            out.append(line)
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM4")
    ap.add_argument("--tx", type=int, default=17, help="ESP32 发 -> ECW RX")
    ap.add_argument("--rx", type=int, default=18, help="ESP32 收 <- ECW TX")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--wait", type=float, default=1.5, help="每条命令等待秒数")
    ap.add_argument("--raw", action="store_true", help="保留日志前缀")
    ap.add_argument("commands", nargs="+")
    args = ap.parse_args()

    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = 115200
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()
    ser.reset_input_buffer()
    time.sleep(0.3)

    # 进桥
    ser.write(f"BRIDGE {args.tx} {args.rx} {args.baud}\r\n".encode())
    ser.flush()
    time.sleep(1.0)
    ser.reset_input_buffer()

    sections = []
    for cmd in args.commands:
        ser.write(cmd.encode() + b"\r")
        ser.flush()
        buf = bytearray()
        t0 = time.time()
        while time.time() - t0 < args.wait:
            d = ser.read(4096)
            if d:
                buf += d
                t0 = time.time()          # 有数据就继续等
        text = buf.decode("utf-8", "replace")
        sections.append((cmd, text))

    # 退桥
    ser.write(b"\x1b")
    ser.flush()
    time.sleep(0.4)
    ser.close()

    for cmd, text in sections:
        print(f"\n========== {cmd} ==========")
        body = text if args.raw else strip_prefix(text)
        # 去掉命令自身的回显行
        lines = [l for l in body.splitlines() if l.strip() != cmd.strip()]
        print("\n".join(lines) if lines else "(无输出)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
