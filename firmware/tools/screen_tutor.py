#!/usr/bin/env python3
"""
屏幕联调陪练 —— 配合 ScreenLink 固件使用。

用法：
  python screen_tutor.py              # 交互式：实时看板子输出，直接敲命令
  python screen_tutor.py SCAN         # 只发一条命令并看 20 秒结果后退出
  python screen_tutor.py PINS 12 11 10 9 8 13
  python screen_tutor.py PING

支持命令（发给板子）：
  HELP / SCAN / PINS sclk mosi cs dc rst busy / PING / TEST / WATCH

端口自动找（CH343 的 COM4 或原生 USB 的 COM5 都行），不用记 COM 几。
"""
import sys
import time
import threading
import serial
import serial.tools.list_ports

BAUD = 115200
VIDS = (0x1A86, 0x303A)          # CH343 / ESP32-S3 原生 USB


def find_port():
    ports = [p.device for p in serial.tools.list_ports.comports() if p.vid in VIDS]
    return ports[0] if ports else None


def main():
    port = find_port()
    if not port:
        print("没找到板子：请确认 USB 线插好，且这根线能传数据（不是纯充电线）。")
        return 1
    print(f"已连接板子：{port} @ {BAUD}")
    print("屏幕联调陪练启动。敲 HELP 看命令，Ctrl+C 退出。\n")

    try:
        s = serial.Serial(port, BAUD, timeout=0.05)
    except Exception as e:
        print(f"打开串口失败：{e}")
        return 1

    stop = threading.Event()

    def reader():
        buf = b""
        while not stop.is_set():
            try:
                if s.in_waiting:
                    buf += s.read(s.in_waiting)
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        text = line.decode("utf-8", "replace").rstrip()
                        if text.strip():
                            print("  " + text)
                time.sleep(0.01)
            except Exception:
                break

    th = threading.Thread(target=reader, daemon=True)
    th.start()

    # 命令行直发模式
    oneshot = " ".join(sys.argv[1:]).strip() if len(sys.argv) > 1 else ""
    if oneshot:
        time.sleep(0.4)
        print(f">> {oneshot}")
        s.write((oneshot + "\r\n").encode())
        seconds = 25 if oneshot.upper().startswith("TEST") else 12
        time.sleep(seconds)
    else:
        try:
            while True:
                cmd = input(">> ").strip()
                if not cmd:
                    continue
                if cmd.lower() in ("quit", "exit", "q"):
                    break
                s.write((cmd + "\r\n").encode())
                time.sleep(0.1)
        except (KeyboardInterrupt, EOFError):
            print()

    stop.set()
    time.sleep(0.15)
    s.close()
    print("\n已退出。板子仍在运行 ScreenLink。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
