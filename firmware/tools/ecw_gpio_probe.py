#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
ecw_gpio_probe.py - 扫 ECW6700 的 GPIO, 找出"被外部器件驱动"的脚

原理（只用 ut_gpio, 不需要任何特殊仪器）:
    把脚设成输出并写 0, 再读回:
        读回 1  ->  外部有更强的上拉/推挽输出在拉高  -> 这个脚上挂了东西
        读回 0  ->  正常被拉低, 说明空着或本来就低
    再把脚写 1, 再读回:
        读回 0  ->  外部被拉到地 (可能接错到 GND)

用途: 不知道传感器接在哪个脚时, 一次把可疑范围全扫掉。

安全: 跳过 GPIO9/10（那是 UART1/ISP, 也是本桥的通路, 驱动它会断链）。
      扫完建议 `reboot` 一次, 让所有脚恢复默认状态。

用法:
    python ecw_gpio_probe.py
    python ecw_gpio_probe.py --pins 19 20 21
"""
import argparse
import re
import sys
import time

import serial

PREFIX = re.compile(r"\[\d{2}:\d{2}:\d{2}\.\d+\]\[[^\]]*\]\[\d+\]")
VALUE = re.compile(r"gpio\[(\d+)\] value = (\d+)")

# 板级文档里存在的外设脚; 刻意排除 9/10(UART1/ISP 桥通路)
DEFAULT_PINS = [
    0, 1, 2, 3, 4, 5, 6, 7, 8,
    11, 12, 13, 14, 15, 16, 17, 18,
    19, 20, 21, 22, 23, 24, 25,
]


def run(ser, cmd, timeout=0.5):
    ser.reset_input_buffer()
    ser.write(cmd.encode() + b"\r")
    ser.flush()
    buf = ""
    t0 = time.time()
    while time.time() - t0 < timeout:
        d = ser.read(256)
        if d:
            buf += d.decode("utf-8", "replace")
            if VALUE.search(PREFIX.sub("", buf)) or "ERROR" in buf or "error" in buf:
                break
    return PREFIX.sub("", buf)


def read_pin(ser, n):
    m = VALUE.search(run(ser, f"ut_gpio read {n}"))
    return int(m.group(2)) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM4")
    ap.add_argument("--tx", type=int, default=17)
    ap.add_argument("--rx", type=int, default=18)
    ap.add_argument("--pins", type=int, nargs="*")
    ap.add_argument("--no-reboot", action="store_true")
    args = ap.parse_args()

    pins = args.pins if args.pins else DEFAULT_PINS

    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = 115200
    ser.timeout = 0.05
    ser.dtr = False
    ser.rts = False
    ser.open()
    ser.reset_input_buffer()
    time.sleep(0.3)

    ser.write(f"BRIDGE {args.tx} {args.rx} 115200\r\n".encode())
    ser.flush()
    time.sleep(1.0)
    ser.reset_input_buffer()

    driven_high, pulled_low, floating = [], [], []
    try:
        for n in pins:
            run(ser, f"ut_gpio write {n} low")
            a = read_pin(ser, n)
            run(ser, f"ut_gpio write {n} high")
            b = read_pin(ser, n)

            if a == 1:
                driven_high.append(n)
                tag = "被外部驱动到 HIGH"
            elif b == 0:
                pulled_low.append(n)
                tag = "被外部拉到 LOW"
            else:
                floating.append(n)
                tag = "空 / 跟随"
            print(f"  GPIO{n:<3} 写0读={a} 写1读={b}   {tag}", flush=True)
    finally:
        if not args.no_reboot:
            print("\n重启 ECW 恢复引脚默认状态...", flush=True)
            ser.write(b"reboot\r")
            ser.flush()
            time.sleep(1.5)
        ser.write(b"\x1b")
        ser.flush()
        time.sleep(0.4)
        ser.close()

    print("\n===== 汇总 =====")
    if driven_high:
        print("  被外部驱动到 HIGH 的脚:", " ".join(f"GPIO{n}" for n in driven_high))
        print("    ^ 这些脚上挂了器件(推挽输出或强上拉), 重点看它们")
    if pulled_low:
        print("  被外部拉到 LOW 的脚:", " ".join(f"GPIO{n}" for n in pulled_low))
    if floating:
        print("  没有外部驱动的脚:", " ".join(f"GPIO{n}" for n in floating))
    return 0


if __name__ == "__main__":
    sys.exit(main())
