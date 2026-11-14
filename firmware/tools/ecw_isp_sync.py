#!/usr/bin/env python3
"""
ECW6700 ISP 下载模式检测 —— 复刻官方 eswin_tool 的 bootrom 同步时序（只读）

官方 eswin_tool.pyc 反查到的流程：
    eswin_download_support.reset()      -> setDTR / setRTS 产生复位时序
    _run_boot_sync_burst()              -> 连续高速发同步字节
    up_chip_sync() / _bootrom_sync()    -> 等 "A5 sync ok" / "sync OK (resp=0xXX)"
    BOOT_SYNC_MAX_SENDS / BOOT_SYNC_READ_TIMEOUT / BOOT_SYNC_TAIL_READ_SIZE
    default_chip_baudrate / boot_baudrate=115200 / download_baudrate=2000000

关键点：bootrom 只在复位后极短窗口内接受同步。
因此本脚本「持续发 0xA5 + 周期性 DTR/RTS 复位」，并提示你在运行期间手动按 RST。

用法：
    python ecw_isp_sync.py                 # 默认 20 秒，自动找 CH340
    python ecw_isp_sync.py --seconds 30
    python ecw_isp_sync.py --port COM6 --baud 115200

只读：只发同步字节，不含擦除/写入命令，不会改动 Flash。
"""

import argparse
import sys
import time

import serial
import serial.tools.list_ports

CH340 = (0x1A86, 0x7523)
SYNC = bytes([0xA5])


def find_port():
    for p in serial.tools.list_ports.comports():
        if (p.vid, p.pid) == CH340:
            return p.device
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=20.0)
    a = ap.parse_args()

    print("=" * 66)
    print(" ECW6700 ISP 同步检测（官方 0xA5 时序 · 只读）")
    print("=" * 66)

    port = a.port or find_port()
    if not port:
        print(" 未发现 CH340 烧录线，请确认 USB 线插好且能传数据。")
        return 1
    print(f" 端口 {port} @ {a.baud}  时长 {a.seconds:.0f}s")
    print(" >>> 脚本运行期间，请按住 BOOT/ISP 键再短按一次 RST <<<")
    print(" （若板上有下载拨码，先拨到下载位再复位）\n")

    try:
        s = serial.Serial(port, a.baud, timeout=0.01)
    except Exception as e:
        print(f" 打开失败: {e}")
        return 1

    t0 = time.monotonic()
    rx_total = 0
    last_reset = 0.0
    reported = False

    try:
        while time.monotonic() - t0 < a.seconds:
            now = time.monotonic() - t0

            # 每 3 秒做一次官方风格的 DTR/RTS 复位脉冲
            if now - last_reset >= 3.0:
                try:
                    s.setDTR(False)
                    s.setRTS(True)
                    time.sleep(0.12)
                    s.setRTS(False)
                    time.sleep(0.05)
                except Exception:
                    pass
                last_reset = now
                print(f" [{now:6.1f}s] 复位脉冲（请趁现在按 RST）")

            # 连续发同步字节（官方 _run_boot_sync_burst 行为）
            try:
                s.write(SYNC * 32)
                s.flush()
            except Exception:
                pass

            try:
                if s.in_waiting:
                    data = s.read(s.in_waiting)
                    if data:
                        rx_total += len(data)
                        print(f" [{now:6.1f}s] ★收到 {len(data)} 字节: {data.hex()}")
                        reported = True
            except Exception:
                pass

            time.sleep(0.01)
    except KeyboardInterrupt:
        print("\n 用户中断")
    finally:
        try:
            s.setDTR(False)
            s.setRTS(False)
            s.close()
        except Exception:
            pass

    print("\n" + "-" * 66)
    if rx_total:
        print(f" 判定: 共收到 {rx_total} 字节 -> 芯片响应了 bootrom 同步")
        print(" 下一步: 可进入编译 allinone -> eswin_tool flash 环节")
        return 0
    print(" 判定: 全程 0 字节 -> 芯片未进下载模式 / 线未接通 / 未供电")
    print(" 排查顺序:")
    print("   1 底板 5V 电源灯是否亮（不亮则一切免谈）")
    print("   2 只按 RST 无效：需按住 BOOT/ISP 再短按 RST，或拨码到下载位")
    print("   3 ISP 座 UART1: IO9=TX / IO10=RX；CH340 的 TX/RX 必须交叉，且共 GND")
    print("   4 换一组杜邦线排除接触不良")
    print("\n 本脚本只发 0xA5 同步字节，不擦除、不写入。")
    return 2


if __name__ == "__main__":
    sys.exit(main())
