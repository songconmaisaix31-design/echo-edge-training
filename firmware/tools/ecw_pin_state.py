#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ECW6700 引脚双向扫描 (通过 ESP32 透传桥)

对每个脚: 内部上拉读一次 + 内部下拉读一次, 用两次读数组合判断外部状态。
  up=1, down=0  -> 悬空 (没有外部连接)
  up=1, down=1  -> 外部强上拉 (模块上拉电阻 / 空闲高电平输出)
  up=0, down=0  -> 外部强下拉 / 器件输出低 (有东西挂在上面)
  up=0, down=1  -> 配置未生效

用法: python ecw_pin_state.py [--pins 19,20,2,17]
"""
import re
import sys
import time

import serial

PORT = 'COM4'
LOCAL_BAUD = 115200
ECW_BAUD = 115200
BRIDGE_TX = 17
BRIDGE_RX = 18

DEFAULT_PINS = [19, 20, 2, 17, 21, 22, 5, 4, 0, 1, 15, 16, 23, 24]

VAL_RE = re.compile(r'gpio\[(\d+)\] value = (\d)')


def send(ser, text, wait=0.45):
    ser.write(text.encode() + b'\r')
    ser.flush()
    buf = bytearray()
    t0 = time.time()
    last = time.time()
    while time.time() - t0 < 2.0:
        d = ser.read(4096)
        if d:
            buf += d
            last = time.time()
        elif time.time() - last > wait:
            break
    return buf.decode('utf-8', 'replace')


def read_pin(ser, pin, pull):
    """pull: 0=up 1=down. 返回电平或 None"""
    send(ser, 'ut_gpio pull %d %d' % (pin, pull), 0.25)
    out = send(ser, 'ut_gpio read %d' % pin, 0.30)
    m = VAL_RE.search(out)
    return int(m.group(2)) if m else None


def main():
    pins = DEFAULT_PINS
    if '--pins' in sys.argv:
        pins = [int(x) for x in sys.argv[sys.argv.index('--pins') + 1].split(',')]

    ser = serial.Serial()
    ser.port = PORT
    ser.baudrate = LOCAL_BAUD
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()
    ser.reset_input_buffer()
    time.sleep(0.3)

    ser.write(('BRIDGE %d %d %d\r\n' % (BRIDGE_TX, BRIDGE_RX, ECW_BAUD)).encode())
    ser.flush()
    time.sleep(1.0)
    ser.reset_input_buffer()

    # I2C 控制器会占用 23/24, 先关掉才能当普通 GPIO 用
    send(ser, 'ut_i2c master_close 0', 0.5)

    print("扫描 %d 个引脚: 上拉读 / 下拉读\n" % len(pins))
    print("%-8s %-6s %-6s  %s" % ("GPIO", "上拉", "下拉", "判定"))
    print("-" * 50)

    rows = []
    for p in pins:
        u = read_pin(ser, p, 0)
        d = read_pin(ser, p, 1)
        send(ser, 'ut_gpio pull %d 2' % p, 0.15)

        if u is None or d is None:
            v = "读不到"
        elif u == 1 and d == 0:
            v = "悬空"
        elif u == 1 and d == 1:
            v = "外部强上拉"
        elif u == 0 and d == 0:
            v = "外部下拉/输出低"
        else:
            v = "未生效"
        rows.append((p, u, d, v))
        print("%-8s %-6s %-6s  %s" % ("IO%d" % p, u, d, v), flush=True)

    ser.write(b'\x1b')
    ser.flush()
    time.sleep(0.4)
    ser.close()

    print("\n=== 判定有外部连接的脚 ===")
    hits = [r for r in rows if r[3] in ("外部强上拉", "外部下拉/输出低")]
    if not hits:
        print("(没有) -> 这些脚全部悬空, 线上没有器件")
    for p, u, d, v in hits:
        print("  IO%-3d  上拉=%s 下拉=%s  -> %s" % (p, u, d, v))


if __name__ == '__main__':
    main()
