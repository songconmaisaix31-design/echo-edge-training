#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
ecw_i2c_scan.py - 通过 ESP32 桥扫 ECW6700 的 I2C 总线

用法:
    python ecw_i2c_scan.py                 # 扫 bus0 和 bus1
    python ecw_i2c_scan.py --bus 0
    python ecw_i2c_scan.py --fast          # 只扫常见传感器地址

原理: ut_i2c master_read 对不存在的地址会打印 "read failed for addr miss-hit",
      有器件会打印 "rx_data" 数据转储。
"""
import argparse
import re
import sys
import time

import serial

PREFIX = re.compile(r"\[\d{2}:\d{2}:\d{2}\.\d+\]\[[^\]]*\]\[\d+\]")

# 常见 I2C 传感器地址 -> 猜测型号
KNOWN = {
    0x38: "AHT20 / AHT10 温湿度",
    0x39: "APDS-9960 光感/接近",
    0x3C: "SSD1306 OLED",
    0x40: "SHT20 / HTU21D / Si7021 温湿度",
    0x44: "SHT30 / SHT31 温湿度",
    0x45: "SHT30(备用地址)",
    0x48: "ADS1115 / LM75 温度",
    0x49: "ADS1115(备用) / TMP102",
    0x4A: "MAX44009 光照",
    0x53: "ADXL345 加速度",
    0x5A: "MLX90614 红外测温",
    0x68: "MPU6050 / DS1307",
    0x69: "MPU6050(备用) / ITG3200",
    0x76: "BMP280 / BME280 气压",
    0x77: "BMP280(备用) / BME280",
    0x23: "BH1750 光照",
    0x29: "TSL2561 光照",
}

COMMON = [0x38, 0x40, 0x44, 0x45, 0x48, 0x23, 0x29, 0x3C,
          0x53, 0x5A, 0x68, 0x69, 0x76, 0x77]


def run(ser, cmd, timeout=0.6, marker="i2c-m,"):
    """发一条命令, 读到 marker 行为止"""
    ser.reset_input_buffer()
    ser.write(cmd.encode() + b"\r")
    ser.flush()
    buf = ""
    t0 = time.time()
    while time.time() - t0 < timeout:
        d = ser.read(512)
        if d:
            buf += d.decode("utf-8", "replace")
            if marker in PREFIX.sub("", buf):
                break
    return PREFIX.sub("", buf)


def scan_bus(ser, bus, addrs, clk=0):
    found = []
    for a in addrs:
        out = run(ser, f"ut_i2c master_read {bus} 1 0x{a:02X} {clk} 1")
        if "rx_data" in out:
            found.append(a)
            print(f"  [命中] 0x{a:02X}  {KNOWN.get(a, '未知器件')}", flush=True)
        elif "read failed" not in out:
            print(f"  0x{a:02X} 无明确应答: {out.strip()[:80]}", flush=True)
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM4")
    ap.add_argument("--tx", type=int, default=17)
    ap.add_argument("--rx", type=int, default=18)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--bus", type=int, default=-1)
    ap.add_argument("--fast", action="store_true")
    args = ap.parse_args()

    addrs = COMMON if args.fast else list(range(0x08, 0x78))
    buses = [args.bus] if args.bus >= 0 else [0, 1]

    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = 115200
    ser.timeout = 0.05
    ser.dtr = False
    ser.rts = False
    ser.open()
    ser.reset_input_buffer()
    time.sleep(0.3)

    ser.write(f"BRIDGE {args.tx} {args.rx} {args.baud}\r\n".encode())
    ser.flush()
    time.sleep(1.0)
    ser.reset_input_buffer()

    total = []
    try:
        for bus in buses:
            print(f"\n===== I2C bus {bus} (SDA=IO23 SCL=IO24) =====", flush=True)
            out = run(ser, f"ut_i2c master_open {bus}", timeout=1.0)
            ok = ("open success" in out) or ("has already opened" in out)
            print("  open:", "OK" if ok else out.strip()[:80], flush=True)
            if not ok:
                print("  该总线打不开, 跳过", flush=True)
                continue
            t0 = time.time()
            hits = scan_bus(ser, bus, addrs)
            print(f"  bus{bus} 扫完, 命中 {len(hits)} 个, 用时 {time.time()-t0:.1f}s", flush=True)
            total += [(bus, a) for a in hits]
            run(ser, f"ut_i2c master_close {bus}", timeout=0.8)
    finally:
        ser.write(b"\x1b")
        ser.flush()
        time.sleep(0.4)
        ser.close()

    print("\n===== 汇总 =====")
    if not total:
        print("  总线上没有应答的器件")
    for bus, a in total:
        print(f"  bus{bus}  0x{a:02X}  {KNOWN.get(a, '未知器件')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
