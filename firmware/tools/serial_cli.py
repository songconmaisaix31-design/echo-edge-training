#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
serial_cli.py - 串口对话小工具（不改 DTR/RTS，不会复位板子）

用法:
  python serial_cli.py COM4 --listen 3
  python serial_cli.py COM4 --send SCAN --wait 8
  python serial_cli.py COM4 --script "HELP|2;SCAN|10;PING|8"

--script 是 "命令|等待秒数" 用分号分隔。
"""
import argparse
import sys
import time

import serial


def open_port(port, baud):
    s = serial.Serial()
    s.port = port
    s.baudrate = baud
    s.timeout = 0.2
    s.write_timeout = 2
    # 关键：不去动 DTR/RTS，避免触发 ESP32 自动复位电路
    s.dtr = False
    s.rts = False
    s.open()
    s.reset_input_buffer()
    return s


def drain(s, seconds):
    t0 = time.time()
    buf = bytearray()
    while time.time() - t0 < seconds:
        chunk = s.read(4096)
        if chunk:
            buf += chunk
            sys.stdout.write(chunk.decode("utf-8", "replace"))
            sys.stdout.flush()
    return bytes(buf)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--listen", type=float, default=0)
    ap.add_argument("--send", default=None)
    ap.add_argument("--wait", type=float, default=5)
    ap.add_argument("--script", default=None)
    args = ap.parse_args()

    s = open_port(args.port, args.baud)
    print(f"[opened {args.port} @ {args.baud}]", flush=True)
    try:
        if args.listen:
            drain(s, args.listen)
        if args.send:
            s.write((args.send + "\r\n").encode())
            s.flush()
            drain(s, args.wait)
        if args.script:
            for item in args.script.split(";"):
                item = item.strip()
                if not item:
                    continue
                cmd, wait = item.rsplit("|", 1)
                print(f"\n>>> {cmd}", flush=True)
                s.write((cmd + "\r\n").encode())
                s.flush()
                drain(s, float(wait))
    finally:
        s.close()
        print("\n[closed]", flush=True)


if __name__ == "__main__":
    main()
