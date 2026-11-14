#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ecw_uart_raw.py —— 通过寄存器直接往 ECW 的 UART0 发送任意字节

原理:
  MEM_BASE_UART0 = 0x20204000
  THR(发送) 偏移 0x20  -> write 0x20204020 0xXX
  LSR(状态) 偏移 0x34  -> bit0 RDR=有数据, bit5 THRE=可发, bit6 TEMT=空闲
  RBR(接收) 偏移 0x20  -> read 0x20204020 4

关键: CLI 每条命令有回显开销, 所以这里不等回复, 按固定间隔连续灌字节,
      把整帧在最短时间内发完, 避免 SYN6288 帧内超时。

用法:
  python ecw_uart_raw.py --hex FD00060100C4E3BAC3A4
  python ecw_uart_raw.py --hex FD00060100C4E3BAC3A4 --gap 6
  python ecw_uart_raw.py --text "你好"        # 自动按 GB2312 打包 SYN6288 帧
  python ecw_uart_raw.py --listen 3           # 只听 3 秒
"""
import argparse
import re
import sys
import time

import serial

PORT = 'COM4'
BAUD = 115200
UART0_BASE = 0x20204000
THR = UART0_BASE + 0x20
LSR = UART0_BASE + 0x34


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--hex', help='要发送的十六进制字节串')
    ap.add_argument('--text', help='按 SYN6288 帧自动打包这段文本(GB2312)')
    ap.add_argument('--gap', type=float, default=8.0, help='字节间隔(毫秒)')
    ap.add_argument('--listen', type=float, default=2.0, help='发完监听多少秒')
    ap.add_argument('--baud', type=int, default=9600)
    args = ap.parse_args()

    if args.text:
        body = args.text.encode('gb2312')
        payload = bytes([0x01, 0x00]) + body
        frame = bytes([0xFD, (len(payload) >> 8) & 0xFF, len(payload) & 0xFF]) + payload
        x = 0
        for b in frame:
            x ^= b
        frame += bytes([x])
    elif args.hex:
        frame = bytes.fromhex(args.hex.replace(' ', '').replace(',', ''))
    else:
        print('需要 --hex 或 --text')
        return 1

    print('待发送 %d 字节: %s' % (len(frame), ' '.join('%02X' % b for b in frame)))

    ser = serial.Serial()
    ser.port = PORT
    ser.baudrate = BAUD
    ser.timeout = 0.02
    ser.dtr = False
    ser.rts = False
    ser.open()
    ser.reset_input_buffer()
    time.sleep(0.3)

    ser.write(b'BRIDGE 17 18 115200\r\n')
    ser.flush()
    time.sleep(1.0)
    ser.reset_input_buffer()

    # 配置 UART0
    ser.write(('ut_uart open 0 %d 0 0\r' % args.baud).encode())
    ser.flush()
    time.sleep(0.6)
    ser.reset_input_buffer()

    # 连续灌字节, 不等回复
    t0 = time.time()
    for b in frame:
        cmd = ('write 0x%08X 0x%02X\r' % (THR, b)).encode()
        ser.write(cmd)
        ser.flush()
        time.sleep(args.gap / 1000.0)
    dt = time.time() - t0
    print('整帧发完, 用时 %.0f ms (平均 %.1f ms/字节)' % (dt * 1000, dt * 1000 / len(frame)))

    # 清掉 CLI 回显, 然后看接收
    time.sleep(0.15)
    ser.reset_input_buffer()

    deadline = time.time() + args.listen
    got = bytearray()
    poll = 0
    while time.time() < deadline:
        # 读 LSR 看有没有数据
        ser.write(('read 0x%08X 4\r' % LSR).encode())
        ser.flush()
        time.sleep(0.05)
        raw = ser.read(8192)
        txt = raw.decode('utf-8', 'replace')
        m = re.search(r'%08X:\s*([0-9A-Fa-f]{8})' % LSR, txt)
        poll += 1
        if m:
            lsr = int(m.group(1), 16)
            if lsr & 0x01:            # RDR: 有数据
                ser.write(('read 0x%08X 4\r' % THR).encode())
                ser.flush()
                time.sleep(0.05)
                raw2 = ser.read(8192)
                t2 = raw2.decode('utf-8', 'replace')
                m2 = re.search(r'%08X:\s*([0-9A-Fa-f]{8})' % THR, t2)
                if m2:
                    b = int(m2.group(1), 16) & 0xFF
                    got.append(b)
                    print('  收到 0x%02X' % b)

    ser.write(b'\x1b')
    ser.flush()
    time.sleep(0.3)
    ser.close()

    print('\n轮询 %d 次, 共收到 %d 字节' % (poll, len(got)))
    if got:
        print('回传: %s' % ' '.join('%02X' % b for b in got))
        CODE = {0x4A: '初始化成功', 0x41: '命令接收成功', 0x45: '命令接收失败',
                0x4E: '正在播报', 0x4F: '空闲'}
        for b in got:
            if b in CODE:
                print('  -> 0x%02X = %s' % (b, CODE[b]))
    else:
        print('没有任何回传')
    return 0


if __name__ == '__main__':
    sys.exit(main())
