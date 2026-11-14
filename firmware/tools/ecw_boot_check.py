#!/usr/bin/env python3
"""
ECW6700 下载模式检测（只读，不写 Flash）

握手协议来源：官方 B02P12 工具包 flash/eswin_tool（PyInstaller 归档内
eswin_tool.pyc）反查到的常量：
    "A5 sync ok" / "A5 sync err" / up_chip_sync / _bootrom_sync /
    _run_boot_sync_burst / setDTR / setRTS / reset / boot_baudrate
=> 官方烧录器用 0xA5 作为 bootrom 同步字节，配合 DTR/RTS 复位进入下载模式。

用法：
    python ecw_boot_check.py                 # 自动找 CH340 口
    python ecw_boot_check.py --port COM6

判定：
    有任意响应  -> 芯片已在 bootrom 下载模式，可以进行后续编译/烧录
    全部静默    -> 未进下载模式 / TX-RX-GND 未接通 / 底板未真正上电
"""

import argparse
import sys
import time

import serial
import serial.tools.list_ports

BAUDS = (9600, 57600, 115200, 230400, 460800, 921600, 1500000, 2000000)
FRAMES = [
    ("A5 x1", bytes([0xA5])),
    ("A5 x8", bytes([0xA5]) * 8),
    ("A5 x32", bytes([0xA5]) * 32),
    ("A5 5A", bytes([0xA5, 0x5A])),
    ("7F", bytes([0x7F])),
]
CH340 = (0x1A86, 0x7523)


def find_port():
    for p in serial.tools.list_ports.comports():
        if (p.vid, p.pid) == CH340:
            return p.device
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="")
    a = ap.parse_args()

    print("=" * 64)
    print(" ECW6700 下载模式检测（只读 · 官方 0xA5 同步协议）")
    print("=" * 64)

    ports = [(p.device, f"{p.vid:04X}:{p.pid:04X}" if p.vid else "-")
             for p in serial.tools.list_ports.comports()]
    print(" 在线串口:", ports if ports else "(空)")

    port = a.port or find_port()
    if not port:
        print("\n 未发现 CH340 烧录线。请确认 USB 线插好且能传数据。")
        return 1
    print(f" 检测端口: {port}")

    # 官方 eswin_download_support.reset 使用 setDTR / setRTS 做复位时序
    try:
        s = serial.Serial(port, 115200, timeout=0.05)
        s.setDTR(False)
        s.setRTS(True)
        time.sleep(0.15)
        s.setRTS(False)
        time.sleep(0.1)
        buf = b""
        t = time.monotonic()
        while time.monotonic() - t < 1.5:
            if s.in_waiting:
                buf += s.read(s.in_waiting)
            time.sleep(0.01)
        s.close()
        print(f" DTR/RTS 复位后被动监听: {len(buf)} 字节 "
              f"{buf[:60].hex() if buf else '(静默)'}")
    except Exception as e:
        print(f" 复位脉冲失败: {e}")

    total = 0
    print("\n 同步探测（8 波特率 x 5 帧）:")
    for b in BAUDS:
        row = []
        for name, frame in FRAMES:
            try:
                s = serial.Serial(port, b, timeout=0.04)
                s.reset_input_buffer()
                s.write(frame)
                s.flush()
                t = time.monotonic()
                buf = b""
                while time.monotonic() - t < 0.25:
                    if s.in_waiting:
                        buf += s.read(s.in_waiting)
                    time.sleep(0.005)
                s.close()
                if buf:
                    total += len(buf)
                    row.append(f"{name}->{buf.hex()[:24]}")
            except Exception:
                pass
        print(f"   {b:8d}: {', '.join(row) if row else '无响应'}")

    print("\n" + "-" * 64)
    if total:
        print(f" 判定: 收到 {total} 字节响应 -> 芯片已进 bootrom 下载模式")
        print(" 下一步: 按官方骨架编译 firmware_allinone.bin 后烧录")
        return 0
    print(" 判定: 全部静默 -> 芯片未进下载模式")
    print(" 按概率排查:")
    print("   1) 只按 RST 不会进下载模式：RST 只是复位，芯片从 Flash 正常启动。")
    print("      需按住 BOOT/ISP 键再短按 RST，或把橙色拨码拨到下载位后复位。")
    print("   2) CH340 的 TX/RX 接反或未共 GND —— 交换 TX/RX 再试。")
    print("   3) 底板 5V 供电未真正到位（板上电源灯不亮即为证）。")
    print("   4) 板间线接到 ESP32 不影响本链路；烧录只走 COM6 这一路。")
    print("\n 注意: 本脚本只发同步字节，不写 Flash、不擦除。")
    return 2


if __name__ == "__main__":
    sys.exit(main())
