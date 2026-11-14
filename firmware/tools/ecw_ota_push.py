#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ecw_ota_push.py —— 通过 WiFi + HTTP 给 ECW6700 更新固件（免下载模式 / 免烧录器）

原理：
  1) 电脑起一个 HTTP 服务，把 OTA 包放出去；
  2) 通过 ESP32 桥把 CLI 命令发给 ECW：wifi_sta 连上同一个网络；
  3) 发 `ota service_ota_start http://<PC-IP>:<端口>/<包名>`；
  4) 芯片自己下载 -> 写入 cpu 分区 -> 重启。

用法：
  python ecw_ota_push.py --ssid MyWiFi --password 12345678 --package ecw-ota/standalone_AA_1.0.0.bin
  python ecw_ota_push.py --ssid MyWiFi --password 12345678 --package xxx.bin --port 8123 --no-serve
"""
import argparse
import functools
import http.server
import os
import re
import socket
import socketserver
import subprocess
import sys
import threading
import time

import serial

PORT = 'COM4'
BAUD = 115200


# ---------------------------------------------------------------- HTTP 服务
class Handler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, fmt, *args):
        sys.stdout.write('  [http] %s\n' % (fmt % args))
        sys.stdout.flush()


def start_server(directory, port, bind):
    handler = functools.partial(Handler, directory=directory)
    httpd = socketserver.TCPServer((bind, port), handler)
    httpd.allow_reuse_address = True
    t = threading.Thread(target=httpd.serve_forever, daemon=True)
    t.start()
    return httpd


def lan_ip(prefer=None):
    if prefer:
        return prefer
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(('8.8.8.8', 80))
        return s.getsockname()[0]
    except Exception:
        return '127.0.0.1'
    finally:
        s.close()


# ---------------------------------------------------------------- 桥通道
class Bridge:
    def __init__(self, port=PORT):
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = BAUD
        self.ser.timeout = 0.05
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.ser.reset_input_buffer()
        time.sleep(0.3)
        self.ser.write(b'BRIDGE 17 18 115200\r\n')
        self.ser.flush()
        time.sleep(1.0)
        self.ser.reset_input_buffer()

    def cmd(self, text, wait=1.0, quiet=0.4):
        self.ser.write((text + '\r').encode())
        self.ser.flush()
        buf = bytearray()
        t0 = time.time()
        last = time.time()
        while time.time() - t0 < wait:
            d = self.ser.read(8192)
            if d:
                buf += d
                last = time.time()
            elif time.time() - last > quiet:
                break
        return buf.decode('utf-8', 'replace').replace('\r', '\n')

    def close(self):
        try:
            self.ser.write(b'\x1b')
            self.ser.flush()
            time.sleep(0.2)
        except Exception:
            pass
        self.ser.close()


def wait_ip(br, seconds=40):
    t_end = time.time() + seconds
    while time.time() < t_end:
        out = br.cmd('ifconfig', 1.6)
        m = re.search(r'name:\[wl\].*?ip_addr:\[([0-9.]+)\]', out, re.S)
        if m and m.group(1) != '0.0.0.0':
            return m.group(1)
        time.sleep(2)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ssid', required=True)
    ap.add_argument('--password', default='')
    ap.add_argument('--package', required=True, help='OTA 包路径')
    ap.add_argument('--port', type=int, default=8123, help='HTTP 端口')
    ap.add_argument('--ip', default=None, help='电脑在局域网里的 IP(默认自动探测)')
    ap.add_argument('--no-serve', action='store_true', help='不自己起 HTTP 服务(已有服务时)')
    ap.add_argument('--skip-wifi', action='store_true', help='跳过连 WiFi(已连上时)')
    a = ap.parse_args()

    pkg_path = os.path.abspath(a.package)
    if not os.path.isfile(pkg_path):
        sys.exit('找不到 OTA 包: %s' % pkg_path)
    pkg_name = os.path.basename(pkg_path)
    pkg_dir = os.path.dirname(pkg_path)
    size = os.path.getsize(pkg_path)

    ip = lan_ip(a.ip)
    url = 'http://%s:%d/%s' % (ip, a.port, pkg_name)
    print('OTA 包 : %s  (%d 字节)' % (pkg_name, size))
    print('本机 IP: %s' % ip)
    print('下载地址: %s' % url)

    if not a.no_serve:
        try:
            start_server(pkg_dir, a.port, '0.0.0.0')
            print('HTTP 服务已启动, 目录 %s' % pkg_dir)
        except OSError as e:
            sys.exit('HTTP 服务起不来(端口被占?): %s' % e)

    br = Bridge()
    try:
        if not a.skip_wifi:
            print('\n[1/4] 切到 sta 模式 ...')
            print(' ', br.cmd('wifi_set_mode 3', 2.0).strip()[:120])
            print('[2/4] 连接 WiFi: %s' % a.ssid)
            cmd = 'wifi_sta %s %s' % (a.ssid, a.password) if a.password else 'wifi_sta %s' % a.ssid
            print(' ', br.cmd(cmd, 3.0).strip()[:200])
            print('[3/4] 等待拿到 IP ...')
            got = wait_ip(br, 45)
            if not got:
                print('  没拿到 IP, 最后状态:')
                print(br.cmd('ifconfig', 2.0))
                sys.exit('WiFi 没连上')
            print('  连上了, 设备 IP = %s' % got)
        print('[3/4] 触发 OTA: %s' % url)
        out = br.cmd('ota service_ota_start %s' % url, 4.0)
        print(out.strip()[:600])
        if 'OTA:download already in progress' in out:
            print('  (上一轮 OTA 还在跑)')
        print('[4/4] 观察 60 秒: 下载进度 / 重启 ...')
        t_end = time.time() + 60
        while time.time() < t_end:
            d = br.ser.read(8192)
            if d:
                sys.stdout.write(d.decode('utf-8', 'replace').replace('\r', '\n'))
                sys.stdout.flush()
            else:
                time.sleep(0.2)
    finally:
        try:
            br.close()
        except Exception:
            pass
        print('\n提示：重启后 CLI 会断一下, 用 `python ecw_bridge_cli.py get_mac` 确认恢复。')


if __name__ == '__main__':
    sys.exit(main())
