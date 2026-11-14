"""Echo Edge 面板 GUI 设计稿渲染器。

目标设备: SSD1306 128x64 单色 OLED (1bpp)
用途: 先在这里把版式、字重、留白定下来, 再导出成固件用的字模与坐标。
固件里没有抗锯齿、没有半透明, 所以这里的所有判断都必须是 1 位决策。
"""
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

W, H = 128, 64
OUT = Path(__file__).resolve().parent

FONT_DIR = "C:/Windows/Fonts"


def cn(text, px=16, weight="regular"):
    name = "msyhbd.ttc" if weight == "bold" else "msyh.ttc"
    return ImageFont.truetype(str(Path(FONT_DIR) / name), px)


# ---------------------------------------------------------------- 5x7 大写
BIG = {
    " ": (0x00, 0x00, 0x00, 0x00, 0x00),
    "-": (0x08, 0x08, 0x08, 0x08, 0x08),
    ".": (0x00, 0x60, 0x60, 0x00, 0x00),
    "/": (0x40, 0x30, 0x08, 0x06, 0x01),
    ":": (0x00, 0x36, 0x36, 0x00, 0x00),
    "%": (0x62, 0x64, 0x08, 0x13, 0x23),
    "0": (0x3E, 0x51, 0x49, 0x45, 0x3E),
    "1": (0x00, 0x42, 0x7F, 0x40, 0x00),
    "2": (0x42, 0x61, 0x51, 0x49, 0x46),
    "3": (0x40, 0x49, 0x49, 0x49, 0x37),
    "4": (0x18, 0x14, 0x12, 0x7F, 0x10),
    "5": (0x27, 0x45, 0x45, 0x45, 0x39),
    "6": (0x3C, 0x4A, 0x49, 0x49, 0x30),
    "7": (0x01, 0x71, 0x09, 0x05, 0x03),
    "8": (0x36, 0x49, 0x49, 0x49, 0x36),
    "9": (0x06, 0x49, 0x49, 0x29, 0x1E),
    "A": (0x7C, 0x12, 0x11, 0x12, 0x7C),
    "B": (0x7F, 0x49, 0x49, 0x49, 0x36),
    "C": (0x3E, 0x41, 0x41, 0x41, 0x41),
    "D": (0x7F, 0x41, 0x41, 0x22, 0x1C),
    "E": (0x7F, 0x49, 0x49, 0x49, 0x41),
    "F": (0x7F, 0x09, 0x09, 0x09, 0x01),
    "G": (0x3E, 0x41, 0x49, 0x49, 0x3A),
    "H": (0x7F, 0x08, 0x08, 0x08, 0x7F),
    "I": (0x00, 0x41, 0x7F, 0x41, 0x00),
    "J": (0x20, 0x40, 0x41, 0x3F, 0x01),
    "K": (0x7F, 0x08, 0x14, 0x22, 0x41),
    "L": (0x7F, 0x40, 0x40, 0x40, 0x40),
    "M": (0x7F, 0x02, 0x04, 0x02, 0x7F),
    "N": (0x7F, 0x02, 0x04, 0x08, 0x7F),
    "O": (0x3E, 0x41, 0x41, 0x41, 0x3E),
    "P": (0x7F, 0x09, 0x09, 0x09, 0x06),
    "Q": (0x3E, 0x41, 0x51, 0x21, 0x5E),
    "R": (0x7F, 0x09, 0x19, 0x29, 0x46),
    "S": (0x46, 0x49, 0x49, 0x49, 0x31),
    "T": (0x01, 0x01, 0x7F, 0x01, 0x01),
    "U": (0x3F, 0x40, 0x40, 0x40, 0x3F),
    "V": (0x1F, 0x20, 0x40, 0x20, 0x1F),
    "W": (0x7F, 0x20, 0x18, 0x20, 0x7F),
    "X": (0x63, 0x14, 0x08, 0x14, 0x63),
    "Y": (0x03, 0x04, 0x78, 0x04, 0x03),
    "Z": (0x61, 0x51, 0x49, 0x45, 0x43),
}

# ---------------------------------------------------------------- 3x5 微字
# 每字符 5 行, 每行 3 位; 打包成 15 位整数, bit0..2 = 第 0 行
MICRO_SRC = {
    "A": (".#.", "#.#", "###", "#.#", "#.#"),
    "B": ("##.", "#.#", "##.", "#.#", "##."),
    "C": (".##", "#..", "#..", "#..", ".##"),
    "D": ("##.", "#.#", "#.#", "#.#", "##."),
    "E": ("###", "#..", "##.", "#..", "###"),
    "F": ("###", "#..", "##.", "#..", "#.."),
    "G": (".##", "#..", "#.#", "#.#", ".##"),
    "H": ("#.#", "#.#", "###", "#.#", "#.#"),
    "I": ("###", ".#.", ".#.", ".#.", "###"),
    "J": ("..#", "..#", "..#", "#.#", ".#."),
    "K": ("#.#", "#.#", "##.", "#.#", "#.#"),
    "L": ("#..", "#..", "#..", "#..", "###"),
    "M": ("#.#", "###", "###", "#.#", "#.#"),
    "N": ("##.", "#.#", "#.#", "#.#", "#.#"),
    "O": (".#.", "#.#", "#.#", "#.#", ".#."),
    "P": ("##.", "#.#", "##.", "#..", "#.."),
    "Q": (".#.", "#.#", "#.#", "##.", ".##"),
    "R": ("##.", "#.#", "##.", "#.#", "#.#"),
    "S": (".##", "#..", ".#.", "..#", "##."),
    "T": ("###", ".#.", ".#.", ".#.", ".#."),
    "U": ("#.#", "#.#", "#.#", "#.#", "###"),
    "V": ("#.#", "#.#", "#.#", "#.#", ".#."),
    "W": ("#.#", "#.#", "###", "###", "#.#"),
    "X": ("#.#", "#.#", ".#.", "#.#", "#.#"),
    "Y": ("#.#", "#.#", ".#.", ".#.", ".#."),
    "Z": ("###", "..#", ".#.", "#..", "###"),
    "0": ("###", "#.#", "#.#", "#.#", "###"),
    "1": (".#.", "##.", ".#.", ".#.", "###"),
    "2": ("##.", "..#", ".#.", "#..", "###"),
    "3": ("##.", "..#", ".#.", "..#", "##."),
    "4": ("#.#", "#.#", "###", "..#", "..#"),
    "5": ("###", "#..", "##.", "..#", "##."),
    "6": (".##", "#..", "###", "#.#", "###"),
    "7": ("###", "..#", ".#.", ".#.", ".#."),
    "8": ("###", "#.#", "###", "#.#", "###"),
    "9": ("###", "#.#", "###", "..#", "##."),
    " ": ("...", "...", "...", "...", "..."),
    ".": ("...", "...", "...", "...", ".#."),
    ":": ("...", ".#.", "...", ".#.", "..."),
    "-": ("...", "...", "###", "...", "..."),
    "/": ("..#", "..#", ".#.", "#..", "#.."),
    ">": ("#..", ".#.", "..#", ".#.", "#.."),
    "=": ("...", "###", "...", "###", "..."),
    "+": ("...", ".#.", "###", ".#.", "..."),
}


def micro_bits(rows):
    value = 0
    for y, row in enumerate(rows):
        for x, ch in enumerate(row):
            if ch == "#":
                value |= 1 << (y * 3 + x)
    return value


MICRO = {k: micro_bits(v) for k, v in MICRO_SRC.items()}


class Canvas:
    def __init__(self):
        self.img = Image.new("1", (W, H), 0)
        self.d = ImageDraw.Draw(self.img)

    def px(self, x, y, on=True):
        if 0 <= x < W and 0 <= y < H:
            self.img.putpixel((int(x), int(y)), 255 if on else 0)

    def rect(self, x, y, w, h, fill=False):
        if fill:
            self.d.rectangle([x, y, x + w - 1, y + h - 1], fill=255)
        else:
            self.d.rectangle([x, y, x + w - 1, y + h - 1], outline=255)

    def hline(self, x0, y, x1):
        self.d.line([x0, y, x1, y], fill=255)

    def vline(self, x, y0, y1):
        self.d.line([x, y0, x, y1], fill=255)

    def text(self, x, y, s, scale=1):
        cx = x
        for ch in s.upper():
            cols = BIG.get(ch, BIG[" "])
            for col, bits in enumerate(cols):
                for row in range(7):
                    if bits & (1 << row):
                        for dy in range(scale):
                            for dx in range(scale):
                                self.px(cx + col * scale + dx, y + row * scale + dy)
            cx += 6 * scale
        return cx

    def micro(self, x, y, s, inverted=False):
        cx = x
        for ch in s.upper():
            bits = MICRO.get(ch, MICRO[" "])
            for row in range(5):
                for col in range(3):
                    if bits & (1 << (row * 3 + col)):
                        self.px(cx + col, y + row, not inverted)
            cx += 4
        return cx

    def micro_width(self, s):
        return len(s) * 4 - 1

    def cjk(self, x, y, s, px=16, weight="regular"):
        layer = Image.new("L", (W, H), 0)
        ImageDraw.Draw(layer).text((x, y), s, font=cn(s, px, weight), fill=255)
        for yy in range(H):
            for xx in range(W):
                if layer.getpixel((xx, yy)) > 96:
                    self.px(xx, yy)
        return x + len(s) * px

    def invert(self, x, y, w, h):
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                if 0 <= xx < W and 0 <= yy < H:
                    cur = self.img.getpixel((xx, yy))
                    self.img.putpixel((xx, yy), 0 if cur else 255)


def logo(c, x, y):
    """产品标记: 超声探头 + 扇形波束。

    11x11 里试过两版才定下来: 方框套心电会糊成 "[3]", 纯心电折线太细。
    扇形是超声设备的通用符号, 而且只有射线和外弧两种笔划, 缩到 11px 仍然
    能读出"从顶点向下张开"的方向性。
    """
    apex = (5, 0)

    # 三条射线: 中轴 + 两侧各 40 度
    for ang in (-40, 0, 40):
        rad = math.radians(ang)
        for r in range(2, 8):
            dx = round(r * math.sin(rad))
            dy = round(r * math.cos(rad))
            c.px(x + apex[0] + dx, y + apex[1] + dy)

    # 外弧: 连接三条射线末端, 形成完整扇面
    for t in range(-5, 6):
        dy = round(math.sqrt(max(0, 49 - t * t)))
        c.px(x + apex[0] + t, y + dy)

    # 顶点实心块 = 探头晶片
    c.rect(x + 4, y, 3, 2, fill=True)


def wifi(c, x, y, level):
    """4 格信号, 从下往上递增。level 0..4"""
    for i in range(4):
        h = 2 + i * 2
        bx = x + i * 4
        by = y + 8 - h
        if i < level:
            c.rect(bx, by, 2, h, fill=True)
        else:
            # 空心 = 无信号, 比全不画更能表达"曾经有"
            c.rect(bx, by, 2, h, fill=False)


def scene(state="ready"):
    c = Canvas()
    valid = state in ("ready", "psax")
    if state == "ready":
        view, label, conf, other, frames, precision = "A4C", "四腔心", 0.92, 0.08, 32, "FP32"
        status, age, level, hint = "LIVE", 2, 4, ""
    elif state == "psax":
        view, label, conf, other, frames, precision = "PSAX", "左室短轴", 0.997, 0.003, 8, "FP32"
        status, age, level, hint = "LIVE", 5, 4, ""
    elif state == "idle":
        view, label, conf, other, frames, precision = "", "待分析", 0.0, 0.0, 0, "--"
        status, age, level, hint = "IDLE", None, 4, "NO RESULT YET"
    elif state == "running":
        view, label, conf, other, frames, precision = "", "分析中", 0.0, 0.0, 0, "--"
        status, age, level, hint = "BUSY", None, 4, "ANALYZING"
    else:
        view, label, conf, other, frames, precision = "", "无连线", 0.0, 0.0, 0, "--"
        status, age, level, hint = "NO NET", None, 0, "CHECK NETWORK"

    # ---- 顶栏: 标记 + 品牌 + 链路 ----
    logo(c, 3, 1)
    c.text(19, 3, "ECHO EDGE", 1)
    wifi(c, 110, 2, level)

    # ---- 品牌小字 + 数据新鲜度 ----
    c.micro(19, 12, "PEDIATRIC ECHO")
    right = "%s %dS" % (status, age) if age is not None else status
    c.micro(W - 4 - c.micro_width(right), 12, right)

    # ---- 分隔 ----
    c.hline(3, 18, 125)

    if not valid:
        # 没有结论时不摆代号、不画空进度条 —— 空条看着像故障, 而"暂无结果"
        # 其实是正常状态。这一屏只回答一件事: 为什么没有结论。
        c.cjk((W - len(label) * 16) // 2, 26, label, 16)
        c.micro((W - c.micro_width(hint)) // 2, 50, hint)
        return c

    # ---- 主结论: 切面代号大字 + 中文全称 ----
    # 代号最多 4 字符(PSAX)。3 字符用 3 倍字, 4 字符降到 2 倍, 否则会和右边
    # 的中文撞上 —— 128px 一行放不下 "PSAX" + "左室短轴" 都用 3 倍字。
    scale = 3 if len(view) <= 3 else 2
    c.text(6, 21 if scale == 3 else 25, view, scale)
    c.cjk(6 + len(view) * 6 * scale + 2, 23, label, 16)

    # ---- 置信度 ----
    bar_x, bar_y, bar_w, bar_h = 6, 44, 94, 8
    c.rect(bar_x, bar_y, bar_w, bar_h, fill=False)
    filled = int((bar_w - 2) * conf)
    if filled > 0:
        c.rect(bar_x + 1, bar_y + 1, filled, bar_h - 2, fill=True)
    c.text(105, 44, "%d" % round(conf * 100), 1)

    # ---- 细节小字 ----
    second = "PSAX" if view == "A4C" else "A4C"
    c.micro(6, 55, "%s %.2f   %dF   %s" % (second, other, frames, precision))

    return c


def save(c, name):
    big = c.img.resize((W * 6, H * 6), Image.NEAREST)
    path = OUT / name
    big.save(path)
    return path


# ------------------------------------------------------------------ 字模导出
CJK_CHARS = "四腔心左室短轴待分析中无连线判定"


def glyph16(ch):
    """把单个汉字渲染成 16x16 点阵。

    按墨迹实际边界裁剪再居中, 不能直接 draw.text((0,0)) —— 微软雅黑的
    16px 字形上下留白不对称, 直接画会让所有字整体偏下 2px。
    """
    pad = 8
    layer = Image.new("L", (16 + pad * 2, 16 + pad * 2), 0)
    ImageDraw.Draw(layer).text((pad, pad), ch, font=cn(ch, 16), fill=255)
    box = layer.getbbox()
    if box is None:
        return [[0] * 16 for _ in range(16)]
    ink = layer.crop(box)
    iw, ih = ink.size
    if iw > 16 or ih > 16:
        scale = min(16 / iw, 16 / ih)
        ink = ink.resize((max(1, int(iw * scale)), max(1, int(ih * scale))),
                         Image.Resampling.LANCZOS)
        iw, ih = ink.size
    cell = Image.new("L", (16, 16), 0)
    cell.paste(ink, ((16 - iw) // 2, (16 - ih) // 2))
    return [[1 if cell.getpixel((x, y)) > 96 else 0 for x in range(16)] for y in range(16)]


def c_array(name, values, ctype="uint8_t", per_line=12, fmt="0x%02X"):
    out = [f"static const {ctype} {name}[] = {{"]
    line = "  "
    for i, v in enumerate(values):
        token = fmt % v if fmt else str(v)
        if len(line) + len(token) + 2 > 96:
            out.append(line.rstrip())
            line = "  "
        line += token + ", "
    if line.strip():
        out.append(line.rstrip())
    out.append("};")
    return "\n".join(out)


def export_header(path):
    blocks = []
    blocks.append("/* panel_assets.h - 由 tools/panel_design.py 生成, 不要手改。\n"
                  " *\n"
                  " * 三套字形解决三件不同的事:\n"
                  " *   micro  3x5   品牌小字、帧数、精度 —— 屏幕上最小的一层信息\n"
                  " *   cjk    16x16 切面中文名, 医生扫一眼就能对上解剖\n"
                  " *   logo   11x11 产品标记, 扇形探头\n"
                  " */\n")
    blocks.append("#pragma once\n#include <stdint.h>\n")

    # micro
    chars = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.:-/>=+"
    bits = [MICRO.get(ch, 0) for ch in chars]
    blocks.append('static const char MICRO_CHARSET[] = "%s";\n' % chars)
    blocks.append(c_array("MICRO_BITS", bits, "uint16_t", 10, "0x%04X") + "\n")

    # logo: 11x11, 打包成 2 字节/行
    c = Canvas()
    logo(c, 0, 0)
    rows = []
    for y in range(11):
        lo = 0
        for x in range(8):
            if c.img.getpixel((x, y)):
                lo |= 1 << x
        hi = 0
        for x in range(8, 11):
            if c.img.getpixel((x, y)):
                hi |= 1 << (x - 8)
        rows.extend([lo, hi])
    blocks.append("/* 11x11, 每行 2 字节: 低字节 x0..x7, 高字节 x8..x10 */")
    blocks.append(c_array("LOGO_BITS", rows, "uint8_t", 11) + "\n")

    # cjk
    blocks.append("/* 16x16, 每字 32 字节, 行优先, 每行 2 字节高位在左 */")
    blocks.append('static const char CJK_CHARSET[] = "%s";\n' % CJK_CHARS)
    flat = []
    for ch in CJK_CHARS:
        grid = glyph16(ch)
        for row in grid:
            flat.append(int("".join(str(v) for v in row[:8]), 2))
            flat.append(int("".join(str(v) for v in row[8:]), 2))
    blocks.append(c_array("CJK_BITS", flat, "uint8_t", 16) + "\n")
    blocks.append("#define CJK_GLYPHS %d" % len(CJK_CHARS))

    path.write_text("\n".join(blocks), encoding="utf-8")
    return path


if __name__ == "__main__":
    for st in ("ready", "psax", "idle", "running", "offline"):
        p = save(scene(st), f"design-{st}.png")
        print(p)
    hdr = export_header(Path(__file__).resolve().parents[1] / "firmware" / "echo-panel" / "panel_assets.h")
    print(hdr)
