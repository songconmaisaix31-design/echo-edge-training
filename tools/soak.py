"""复位后盯住设备, 量"从开机到 WiFi 连上"和"连上之后轮询有没有丢"。

单看一次 STATUS 说明不了稳定性: 每次打开串口都会复位, 8 秒的快照很可能
正好落在 WiFi 还没连上的空窗里, 看起来像"坏了"。这里改成轮询等待, 并且
把等待时长记下来, 几次复位对比才能看出是不是真的在退化。

用法: python soak.py [复位次数] [每次连上后观察秒数]
"""
import re
import sys
import time

import serial

PORT, BAUD = "COM4", 115200


def drain(ser, seconds):
    buf = b""
    deadline = time.time() + seconds
    while time.time() < deadline:
        chunk = ser.read(512)
        if chunk:
            buf += chunk
    return buf.decode("utf-8", "replace")


def panel_block(text):
    idx = text.rfind("---- PANEL ----")
    return text[idx:] if idx >= 0 else text


def field(block, key):
    for line in block.splitlines():
        if key in line:
            return line.split(":", 1)[-1].strip() if ":" in line else line.strip()
    return ""


def one_cycle(ser, observe):
    """复位 -> 计时到联网 -> 观察 observe 秒 -> 返回统计"""
    ser.dtr = False
    ser.rts = False
    time.sleep(0.2)
    ser.setDTR(False)          # 保持不复位
    ser.setRTS(True)
    time.sleep(0.15)
    ser.setRTS(False)
    t0 = time.time()
    ser.reset_input_buffer()

    # 等 WiFi
    boot_log = ""
    connected_at = None
    while time.time() - t0 < 30:
        boot_log += drain(ser, 1.0)
        if "已连接" in boot_log and "IP" in boot_log:
            connected_at = time.time() - t0
            break

    ser.write(b"STATUS\r\n")
    ser.flush()
    first = panel_block(drain(ser, 2.0))

    # 连上之后再观察一段时间, 看轮询有没有失败
    time.sleep(observe)
    ser.write(b"STATUS\r\n")
    ser.flush()
    second = panel_block(drain(ser, 2.0))

    return connected_at, boot_log, first, second


def main():
    cycles = int(sys.argv[1]) if len(sys.argv) > 1 else 2
    observe = float(sys.argv[2]) if len(sys.argv) > 2 else 30

    ser = serial.Serial(PORT, BAUD, timeout=0.2)
    ser.dtr = False
    ser.rts = False
    time.sleep(0.3)

    for i in range(1, cycles + 1):
        conn, log, first, second = one_cycle(ser, observe)
        print("=" * 60)
        print("第 %d 次复位" % i)
        print("  联网耗时 : %s" % ("%.1f s" % conn if conn is not None else "30 秒内未连上"))
        print("  开机日志 : %s" % " / ".join(
            ln.strip() for ln in log.splitlines() if ln.strip())[-300:])
        print("  刚连上   : %s" % field(first, "状态"))
        print("  观察后   : %s" % field(second, "状态"))
        print("  轮询     : %s" % field(second, "轮询"))
        print("  堆       : %s" % field(second, "运行"))
        print("  屏       : %s" % field(second, "屏"))
        print("  WiFi     : %s" % field(second, "WiFi"))
    ser.close()


if __name__ == "__main__":
    main()
