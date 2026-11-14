"""把设备上的 1KB 帧缓冲拉下来还原成 PNG。

屏是实物, 版式对不对(中文和英文大字有没有贴在一起、右边百分比有没有越界)
在串口日志里看不出来。DUMP 命令把 oled.fb 原样倒出, 这里照着 SSD1306 的
页寻址格式(每页 8 行, 1=点亮)画回图, 就是屏上真实的样子。

用法: python dump_screen.py [out.png]
"""
import sys
import time

import serial
from PIL import Image

PORT, BAUD = "COM4", 115200
W, H = 128, 64
SCALE = 4


def read_until(ser, marker, timeout):
    buf = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        chunk = ser.read(256)
        if chunk:
            buf += chunk
            if marker in buf:
                break
    return buf


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "screen-live.png"

    ser = serial.Serial(PORT, BAUD, timeout=0.2)
    ser.dtr = False
    ser.rts = False
    time.sleep(0.3)
    ser.reset_input_buffer()

    # 打开串口会复位, 等它重连 WiFi 并完成首轮拉取
    time.sleep(8)
    ser.reset_input_buffer()
    ser.write(b"DUMP\r\n")
    ser.flush()

    raw = read_until(ser, b"FBEND", 6)
    ser.close()

    text = raw.decode("utf-8", "replace")
    lines = [ln.strip() for ln in text.splitlines() if ln.strip()]
    pages = [ln for ln in lines if len(ln) == W * 2 and all(c in "0123456789ABCDEF" for c in ln)]
    if len(pages) != H // 8:
        raise SystemExit("帧缓冲没读全: 期望 %d 页, 实际 %d 页\n%s"
                         % (H // 8, len(pages), text[-400:]))

    img = Image.new("RGB", (W, H), (0, 0, 0))
    px = img.load()
    for page, line in enumerate(pages):
        for x in range(W):
            byte = int(line[x * 2:x * 2 + 2], 16)
            for bit in range(8):
                if byte & (1 << bit):
                    px[x, page * 8 + bit] = (255, 255, 255)

    img.resize((W * SCALE, H * SCALE), Image.NEAREST).save(out)
    lit = sum(1 for y in range(H) for x in range(W) if px[x, y] != (0, 0, 0))
    print("%s  %dx%d  点亮 %d 像素 (%.1f%%)" % (out, W, H, lit, lit * 100.0 / (W * H)))


if __name__ == "__main__":
    main()
