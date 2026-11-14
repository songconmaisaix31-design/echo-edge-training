#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ecw_uart_listen.py —— 监听 ECW 的 UART0 接收端（寄存器级，不阻塞 CLI）

先 open UART0，然后循环轮询 LSR 的 RDR 位；有数据就读 RBR。
用途：SYN6288 重新上电会主动发 0x4A，用这个能抓到，从而确认模块在线且 TX 接对。

用法: python ecw_uart_listen.py --seconds 180 --baud 9600
"""
import argparse
import re
import sys
import time

import serial

PORT = 'COM4'
LSR = 0x20204034
RBR = 0x20204020
CODE = {0x4A: '初始化成功', 0x41: '命令接收成功', 0x45: '命令接收失败',
        0x4E: '正在播报', 0x4F: '空闲'}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--seconds', type=float, default=180)
    ap.add_argument('--baud', type=int, default=9600)
    ap.add_argument('--out', default='uart0_listen.log')
    a = ap.parse_args()

    ser = serial.Serial()
    ser.port = PORT
    ser.baudrate = 115200
    ser.timeout = 0.03
    ser.dtr = False
    ser.rts = False
    ser.open()
    ser.reset_input_buffer()
    time.sleep(0.3)

    ser.write(b'BRIDGE 17 18 115200\r\n')
    ser.flush()
    time.sleep(1.0)
    ser.reset_input_buffer()

    def cmd(t, wait=0.12):
        ser.write((t + '\r').encode())
        ser.flush()
        time.sleep(wait)
        return ser.read(8192).decode('utf-8', 'replace')

    cmd('ut_uart close 0', 0.3)
    out = cmd('ut_uart open 0 %d 0 0' % a.baud, 0.5)
    ok = 'open ok' in out

    log = open(a.out, 'w', encoding='utf-8')
    log.write('open uart0 @%d -> %s\n' % (a.baud, 'OK' if ok else 'FAILED'))
    log.flush()
    print('UART0 open: %s  监听 %.0f 秒, 现在可以把 TTS 模块拔下来再插回去' %
          ('OK' if ok else '失败', a.seconds), flush=True)

    t_end = time.time() + a.seconds
    got = []
    polls = 0
    while time.time() < t_end:
        r = cmd('read 0x%08X 4' % LSR, 0.1)
        polls += 1
        m = re.search(r'%08X:\s*([0-9A-Fa-f]{8})' % LSR, r)
        if not m:
            continue
        if int(m.group(1), 16) & 0x01:
            r2 = cmd('read 0x%08X 4' % RBR, 0.1)
            m2 = re.search(r'%08X:\s*([0-9A-Fa-f]{8})' % RBR, r2)
            if m2:
                b = int(m2.group(1), 16) & 0xFF
                got.append(b)
                line = '[%6.1fs] 0x%02X %s' % (
                    a.seconds - (t_end - time.time()), b, CODE.get(b, ''))
                print(line, flush=True)
                log.write(line + '\n')
                log.flush()

    ser.write(b'\x1b')
    ser.flush()
    time.sleep(0.3)
    ser.close()

    summary = '\n轮询 %d 次, 收到 %d 字节: %s\n' % (
        polls, len(got), ' '.join('%02X' % b for b in got) if got else '(无)')
    print(summary, flush=True)
    log.write(summary)
    log.close()


if __name__ == '__main__':
    sys.exit(main())
