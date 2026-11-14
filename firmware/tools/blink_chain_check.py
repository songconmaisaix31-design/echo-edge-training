#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
控制链路自检 —— 不依赖任何 IDE，只用串口验证「PC -> 板子 -> LED」是否通。

适用硬件（本机实测）：ESP32-S3-DevKitC-1，CH343 转串口，115200 8N1，
板载 1 颗 WS2812 接 GPIO48。

自动识别两种固件：
  - 出厂 MicroPython RGB Demo : "RGB Demo" + "50%R/50%G/50%B"
  - 自编译 Arduino Blink      : "Blink | ESP32-S3" + "LED ON/LED OFF"

分层检查：
  1. 串口物理链路        PC <-> USB-UART <-> 芯片
  2. 复位/自动下载电路    RTS 能否复位芯片
  3. 从 Flash 正常启动    boot:0x8 SPI_FAST_FLASH_BOOT
  4. 固件运行             应用启动横幅
  5. LED 驱动             状态行 + 翻转周期（Blink 还检查亮灭交替）
  6. 固件完整性           SHA-256 校验告警
  7. 编译链路             运行中的固件是否为自编译产物
  8. 烧录链路(--deep)     esptool 能否进 ROM bootloader 读芯片/Flash

用法:
  python blink_chain_check.py
  python blink_chain_check.py --port COM4 --seconds 7 --deep
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("缺少 pyserial，请先执行: python -m pip install pyserial")

BAUD = 115200
RGB_RE = re.compile(r"^50%[RGB]$")
BLINK_RE = re.compile(r"LED (ON|OFF)")
SEP = "-" * 66


def hard_reset(ser: serial.Serial) -> None:
    """RTS 脉冲 = EN 低电平复位；DTR 保持 False 让 IO0 为高，从 Flash 启动。"""
    ser.setDTR(False)
    ser.setRTS(False)
    time.sleep(0.05)
    ser.setRTS(True)
    time.sleep(0.15)
    ser.setRTS(False)
    ser.reset_input_buffer()


def capture(ser: serial.Serial, seconds: float):
    """按行采集，(毫秒时间戳, 文本)。"""
    rows, buf, t0 = [], b"", time.monotonic()
    while time.monotonic() - t0 < seconds:
        waiting = ser.in_waiting
        if waiting:
            buf += ser.read(waiting)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                text = raw.decode("utf-8", "replace").strip()
                if text:
                    rows.append(((time.monotonic() - t0) * 1000.0, text))
        else:
            time.sleep(0.01)
    return rows


def deep_probe(port: str) -> str:
    """通过 ROM bootloader 读芯片型号与 Flash，验证烧录通道是否可用。"""
    cmd = [sys.executable, "-m", "esptool", "--port", port,
           "--baud", str(BAUD), "flash-id"]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        return r.stdout + r.stderr
    except Exception as exc:  # noqa: BLE001
        return f"esptool 调用失败: {exc}"


def main() -> int:
    ap = argparse.ArgumentParser(description="控制链路自检 (无需 IDE)")
    ap.add_argument("--port", default="COM4", help="串口号，默认 COM4")
    ap.add_argument("--seconds", type=float, default=7.0, help="监听秒数，默认 7")
    ap.add_argument("--deep", action="store_true", help="额外用 esptool 验证烧录通道")
    args = ap.parse_args()

    print(f"控制链路自检  {args.port} @ {BAUD} 8N1  {args.seconds:g}s")
    print("=" * 66)

    try:
        ser = serial.Serial(args.port, BAUD, timeout=0.05)
    except Exception as exc:  # noqa: BLE001
        print(f"[FAIL] 串口打不开: {exc}")
        return 1

    try:
        hard_reset(ser)
        rows = capture(ser, args.seconds)
    finally:
        ser.close()

    for ms, line in rows:
        print(f"[{ms:7.0f} ms] {line}")
    print(SEP)

    lines = [t for _, t in rows]
    rom = next((t for t in lines if "ESP-ROM:" in t), "")
    boot = next((t for t in lines if "boot:" in t), "")
    sha_bad = any("SHA-256 comparison failed" in t for t in lines)

    # 识别当前固件
    is_blink = any("Blink |" in t for t in lines)
    banner = next((t for t in lines
                   if ("Blink |" in t) or ("RGB Demo" in t)), "")
    fw_name = "Arduino Blink (自编译)" if is_blink else "MicroPython RGB Demo (出厂)"

    if is_blink:
        led = [(ms, BLINK_RE.search(t).group(1))
               for ms, t in rows if BLINK_RE.search(t)]
    else:
        led = [(ms, t) for ms, t in rows if RGB_RE.match(t)]

    results = []

    def check(name, ok, detail):
        results.append((name, ok, detail))

    check("串口物理链路", bool(rows), f"收到 {len(rows)} 行")
    check("复位/自动下载电路", bool(rom), rom or "未捕获到 ROM 启动横幅")
    check("从 Flash 启动", "SPI_FAST_FLASH_BOOT" in boot, boot or "未捕获 boot 行")
    check("固件运行", bool(banner), f"{fw_name} / {banner}")

    intervals = [led[i][0] - led[i - 1][0] for i in range(1, len(led))]
    if intervals:
        avg = sum(intervals) / len(intervals)
        spread = max(intervals) - min(intervals)
        states = [s for _, s in led]
        alternates = all(states[i] != states[i - 1] for i in range(1, len(states)))
        ok = len(led) >= 3 and 800 <= avg <= 1200 and (not is_blink or alternates)
        suffix = ("" if not is_blink else
                  (" [亮灭交替 OK]" if alternates else " [状态未交替!]"))
        check("LED 驱动", ok,
              f"{len(led)} 次状态变化, 平均 {avg:.0f} ms, 抖动 {spread:.0f} ms{suffix}")
    else:
        check("LED 驱动", False, "未捕获到 LED 状态行")

    check("固件完整性", not sha_bad,
          "启动时 SHA-256 校验失败，bootloader 强行启动" if sha_bad
          else "启动时无 SHA-256 校验告警")
    # 编译链路是信息项：出厂固件时不算失败
    if is_blink:
        check("编译链路", True, "运行中的固件是自编译产物")

    print()
    print("判定")
    print(SEP)
    for name, ok, detail in results:
        tag = "WARN" if name == "固件完整性" and not ok else ("PASS" if ok else "FAIL")
        print(f"  [{tag}] {name:<18} {detail}")
    if not is_blink:
        print("  [ -- ] 编译链路           当前为出厂固件，尚未验证编译产物")

    deep_ok = None
    if args.deep:
        print()
        print("烧录通道探测 (esptool)")
        print(SEP)
        out = deep_probe(args.port)
        keys = ("Detecting chip type", "Chip type:", "Features:",
                "Detected flash size", "Connected to")
        for line in out.splitlines():
            if any(k in line for k in keys):
                print("  " + line.strip())
        deep_ok = "ESP32-S3" in out and "Detected flash size" in out
        print(f"  [{'PASS' if deep_ok else 'FAIL'}] ROM bootloader 与 Flash 读写通道")

    failed = sum(1 for name, ok, _ in results
                 if not ok and name != "固件完整性")
    print()
    print("结论: " + ("控制链路全通" if failed == 0 and deep_ok is not False
                     else f"有 {failed} 层未通过"))
    return 0 if failed == 0 and deep_ok is not False else 1


if __name__ == "__main__":
    raise SystemExit(main())
