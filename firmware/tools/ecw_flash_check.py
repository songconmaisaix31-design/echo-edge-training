#!/usr/bin/env python3
"""
ECW6700 烧录链路分层自检（只读，不写 Flash）

用法：
  python ecw_flash_check.py                 # 自动找 CH340 口并全量检查
  python ecw_flash_check.py --port COM6     # 指定端口
  python ecw_flash_check.py --seconds 2     # 缩短监听时间

分层判定：
  L1 USB 枚举        CH340 是否被 Windows 识别
  L2 串口可打开      能否以给定波特率打开
  L3 板侧响应        被动监听 + 安全同步字节握手（不写 Flash）
  L4 厂商工具链      eswin_tool.exe / all-in-one 产物 / catalog 资产是否就绪

L3 无响应时，按概率给中文排查建议。
"""
import argparse
import json
import sys
import time
from pathlib import Path

import serial
import serial.tools.list_ports

WORK = Path(__file__).resolve().parent
MOCE = WORK / "moce"

# CH340 = 通用 USB-TTL（ECW6700 的 ISP/UART 下载线）
# CH343 = ESP32-S3 开发板板载 USB-UART
KNOWN = {
    (0x1A86, 0x7523): "CH340 USB-TTL（ECW6700 烧录线候选）",
    (0x1A86, 0x55D3): "CH343（ESP32-S3 主板 UART0）",
    (0x303A, 0x1001): "ESP32-S3 原生 USB Serial/JTAG",
}

BAUDS = (9600, 57600, 115200, 230400, 921600)
PROBES = [
    ("0x7F 同步", b"\x7f"),
    ("0x55 0xAA", b"\x55\xaa"),
    ("0xAA 0x55", b"\xaa\x55"),
    ("AT\\r\\n", b"AT\r\n"),
]

PASS, FAIL, WARN = "PASS", "FAIL", "未就绪"


def line():
    print("-" * 66)


def list_ports():
    print("\n[L1] USB 串口枚举")
    found = []
    for p in serial.tools.list_ports.comports():
        who = KNOWN.get((p.vid, p.pid), "未知设备") if p.vid and p.pid else "无 VID/PID"
        vidpid = f"{p.vid:04X}:{p.pid:04X}" if p.vid and p.pid else "-"
        print(f"  {p.device:6s} {vidpid:12s} {who}")
        found.append((p.device, p.vid, p.pid, who))
    if not found:
        print("  (没有任何串口设备)")
    ch340 = [x for x in found if x[1:3] == (0x1A86, 0x7523)]
    print(f"  → CH340 烧录线: {', '.join(x[0] for x in ch340) if ch340 else '未发现'}")
    return found, ch340


def probe(port, seconds):
    print(f"\n[L2/L3] {port} 串口可打开性 + 板侧响应")

    opened = []
    for b in BAUDS:
        try:
            s = serial.Serial(port, b, timeout=0.05)
            s.close()
            opened.append(b)
        except Exception:
            pass
    print(f"  可打开波特率: {opened if opened else '无'}  → L2 {PASS if opened else FAIL}")
    if not opened:
        return False

    # 被动监听
    for b in (115200, 921600):
        try:
            s = serial.Serial(port, b, timeout=0.05)
            t = time.monotonic()
            buf = b""
            while time.monotonic() - t < seconds:
                if s.in_waiting:
                    buf += s.read(s.in_waiting)
                time.sleep(0.01)
            s.close()
            print(f"  被动监听 {b:7d}: {len(buf):4d} 字节 "
                  f"{repr(buf[:60]) if buf else '(静默)'}")
        except Exception as e:
            print(f"  被动监听 {b:7d}: 错误 {e}")

    # 安全握手（只发同步字节，不含擦写命令）
    any_resp = False
    for b in (115200, 921600):
        for name, data in PROBES:
            try:
                s = serial.Serial(port, b, timeout=0.05)
                s.reset_input_buffer()
                s.write(data)
                s.flush()
                t = time.monotonic()
                buf = b""
                while time.monotonic() - t < 0.3:
                    if s.in_waiting:
                        buf += s.read(s.in_waiting)
                    time.sleep(0.01)
                s.close()
                if buf:
                    any_resp = True
                    print(f"  {b} {name:12s} → 响应 {repr(buf[:40])}")
            except Exception:
                pass
    if not any_resp:
        print("  安全握手: 全部无响应")

    # 复位脉冲
    try:
        s = serial.Serial(port, 115200, timeout=0.05)
        s.setDTR(False)
        s.setRTS(True)
        time.sleep(0.15)
        s.setRTS(False)
        t = time.monotonic()
        buf = b""
        while time.monotonic() - t < 1.5:
            if s.in_waiting:
                buf += s.read(s.in_waiting)
            time.sleep(0.01)
        s.close()
        print(f"  DTR/RTS 复位后: {len(buf)} 字节 {repr(buf[:60]) if buf else '(静默)'}")
        if buf:
            any_resp = True
    except Exception as e:
        print(f"  复位脉冲失败: {e}")

    print(f"  → L3 板侧响应: {PASS if any_resp else FAIL}")
    return any_resp


def toolchain():
    print("\n[L4] 厂商工具链就绪度")
    exe = list(MOCE.rglob("eswin_tool*")) if MOCE.exists() else []
    bins = list(MOCE.rglob("*allinone*.bin")) if MOCE.exists() else []
    print(f"  eswin_tool.exe : {'已找到 ' + str(exe[0]) if exe else '未安装'}")
    print(f"  all-in-one 产物: {'已找到 ' + str(bins[0]) if bins else '未生成'}")

    cat = MOCE / "catalog" / "toolchains.json"
    asset = None
    if cat.exists():
        d = json.loads(cat.read_text(encoding="utf-8"))
        asset = next((a for a in d.get("assets", []) if a["id"] == "ecw6700-windows"), None)
    if asset:
        print(f"  catalog 资产  : ecw6700-windows / {asset['bytes']/1048576:.0f} MB / "
              f"SDK {asset['versions']['sdk']} / {asset['versions']['programmer']}")
        print(f"  下载地址可用  : {asset['url'].split('/')[2]}")
    print(f"  → L4 工具链: {PASS if exe and bins else WARN}")

    # MOCE 源码里的已知限制
    fl = MOCE / "backend" / "flashing.py"
    if fl.exists():
        t = fl.read_text(encoding="utf-8", errors="replace")
        print("\n  MOCE 源码中对该目标的已知限制：")
        if "not expose a validated no-write identity handshake" in t:
            print("    · plan 阶段不做芯片身份握手（ECW 无 identity）")
        if "coverage unproven" in t:
            print("    · 写后校验记为 coverage unproven（覆盖未证明）")
        if "'application': 'not-tested'" in t or "application" in t:
            print("    · 应用功能恒为 not-tested；reset 恒为 manual")
    return bool(exe and bins)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="")
    ap.add_argument("--seconds", type=float, default=2.0)
    a = ap.parse_args()

    print("=" * 66)
    print(" ECW6700 烧录链路分层自检（只读，不写 Flash）")
    print("=" * 66)

    found, ch340 = list_ports()
    port = a.port or (ch340[0][0] if ch340 else "")
    if not port:
        print("\n没有可用于 ECW6700 的 CH340 串口，检查结束。")
        return 1

    ok = probe(port, a.seconds)
    toolchain()

    line()
    print(" 结论")
    line()
    print(f"  L1 USB 枚举    : {PASS}  ({port})")
    print(f"  L2 串口可打开  : {PASS}")
    print(f"  L3 板侧响应    : {PASS if ok else FAIL}")
    print(f"  L4 厂商工具链  : {WARN}")
    if not ok:
        print("\n  板侧零响应，按概率排查：")
        print("   1) 底板未供电 —— CH340 枚举只证明 USB 端有电，不代表底板有 5V")
        print("   2) 未进入下载模式 —— ECW6700 需按住 BOOT/ISP 再复位（查板子 RST 旁按键）")
        print("   3) TX/RX 接反或未共 GND —— 交换 TX/RX 重试，GND 必须共")
        print("   4) 只接了 USB 端，杜邦线另一端没接到底板 ISP 口")
        print("\n  注意：这块是 CH340 USB-TTL 串口下载线，不是 J-Link/SWD 调试器，")
        print("        不能单步调试，只能靠 eswin_tool 做 all-in-one 写入。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
