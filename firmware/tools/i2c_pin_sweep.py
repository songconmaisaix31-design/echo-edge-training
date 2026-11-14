#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
I2C 引脚组合全扫: 通过串口发 PINS sda scl, 遍历 ESP32-S3 上所有可用 GPIO 组合,
看哪一个组合能扫到 I2C 设备(OLED 一般是 0x3C / 0x3D)。

用途: 不知道屏到底接在哪个脚上时, 让固件自己找出来。

用法:
    python i2c_pin_sweep.py --port COM4
    python i2c_pin_sweep.py --port COM4 --quick     # 只试常见组合
"""
import argparse
import itertools
import re
import sys
import time

import serial

# ESP32-S3-DevKitC-1 (N16R8) 可用作 I2C 的 GPIO
# 排除: 19/20 = USB D-/D+, 26-37 = Flash / 八线 PSRAM, 0 = BOOT
CANDIDATES = (
    [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21]
    + [38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48]
)

QUICK = [
    (8, 9), (9, 8), (4, 5), (5, 4), (6, 7), (7, 6),
    (1, 2), (2, 1), (10, 11), (11, 10), (15, 16), (16, 15),
    (41, 42), (42, 41), (47, 48), (48, 47), (21, 47), (47, 21),
]

FOUND_RE = re.compile(r"找到\s*0x([0-9A-Fa-f]{2})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM4")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--dwell", type=float, default=0.45,
                    help="每个组合等待秒数")
    args = ap.parse_args()

    if args.quick:
        pairs = QUICK
    else:
        pairs = []
        for a, b in itertools.combinations(CANDIDATES, 2):
            pairs.append((a, b))
            pairs.append((b, a))

    print(f"共 {len(pairs)} 个组合, 预计 {len(pairs)*args.dwell/60:.1f} 分钟")

    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = args.baud
    ser.timeout = 0.05
    ser.dtr = False
    ser.rts = False
    ser.open()
    ser.reset_input_buffer()

    hits = []
    try:
        for i, (sda, scl) in enumerate(pairs, 1):
            ser.reset_input_buffer()
            ser.write(f"PINS {sda} {scl}\r\n".encode())
            ser.flush()

            buf = b""
            t0 = time.time()
            while time.time() - t0 < args.dwell:
                d = ser.read(4096)
                if d:
                    buf += d

            text = buf.decode("utf-8", "replace")
            m = FOUND_RE.search(text)
            if m:
                addr = m.group(1)
                hits.append((sda, scl, addr))
                print(f"  [命中] SDA=GPIO{sda} SCL=GPIO{scl} -> 0x{addr}")
                # 保留这个组合, 不再改
                break

            if i % 20 == 0:
                print(f"  ... {i}/{len(pairs)}  (SDA={sda} SCL={scl})", flush=True)

        if hits:
            sda, scl, addr = hits[0]
            print(f"\n结论: 屏在 SDA=GPIO{sda}, SCL=GPIO{scl}, 地址 0x{addr}")
            ser.write(f"PINS {sda} {scl}\r\n".encode())
            time.sleep(1.0)
            ser.write(b"SCAN\r\n")
            time.sleep(1.5)
            out = ser.read(8192).decode("utf-8", "replace")
            print(out)
        else:
            print("\n结论: 所有组合都没有 I2C 应答。")
            print("  -> 屏没有供电(VCC/GND), 或四根线没插到底, 或杜邦线断了")
    finally:
        ser.close()


if __name__ == "__main__":
    sys.exit(main())
