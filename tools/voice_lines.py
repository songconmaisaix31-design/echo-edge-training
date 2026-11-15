"""把播报用的中文片段在编译期转成 GB2312 字节数组。

为什么不在设备上转: UTF-8 -> GB2312 需要一张几千字的码表, 塞进 flash 不值当,
而且运行期转换一旦漏字就是哑音。SYN6288E 吃的是 GB2312/GBK 字节流, 所以这里
在打包阶段就把字节定死, 固件里只有 const 数组。

句子是拼出来的, 不是一句一条:
    "本次影像判断为" + "左室短轴切面" + "，置信度" + "很高"
这样加一个切面只要加一个词条, 不用把组合数乘出来。拼在设备端做, 拼的仍然是
GB2312 字节, 对模块来说就是一段普通文本。

用法: python tools/voice_lines.py
输出: firmware/echo-panel/voice_lines.h
"""
from pathlib import Path

# 键名 -> 中文。键名会变成 VO_<KEY> 数组名, 改键名要同步改固件里的引用。
LINES = {
    # 结论句式: 头 + 切面 + 置信度头 + 等级
    "HEAD": "本次影像判断为",
    "CONF_HEAD": "，置信度",
    "VIEW_A4C": "四腔心切面",
    "VIEW_PSAX": "左室短轴切面",
    "CONF_HIGH": "很高",
    "CONF_MID": "较高",
    "CONF_LOW": "一般",
    # 非结论态
    "IDLE": "还没有分析结果，请先选择一段影像",
    "RUNNING": "正在分析影像，请稍候",
    "STALE": "上一次的分析结果已经过期，请重新分析",
    "OFFLINE": "与工作站的连接已经断开",
    "HELLO": "回声边缘床旁终端已就绪",
}

OUT = Path(__file__).resolve().parents[1] / "firmware" / "echo-panel" / "voice_lines.h"

HEADER = """/*
 * voice_lines.h - 播报文本的 GB2312 字节, 由 tools/voice_lines.py 生成, 不要手改。
 *
 * SYN6288E 只认 GB2312/GBK, 而源码是 UTF-8。运行期转码要带整张码表, 所以
 * 在打包阶段就转好: 这里每个数组都是一段可以直接拼进 TTS 帧的中文文本。
 */
#pragma once
#include <stdint.h>
"""


def main():
    blocks = [HEADER]
    table = []

    for key, text in LINES.items():
        raw = text.encode("gb2312")          # 有字转不出来就直接报错, 别留到设备上
        body = ",".join("0x%02X" % b for b in raw)
        blocks.append("\n// %s (%d 字节)\nstatic const uint8_t VO_%s[] = {%s};\n"
                      % (text, len(raw), key, body))
        table.append((key, text, len(raw)))

    # 拼装用的索引表: 固件按 key 取数组和长度, 避免到处写 sizeof
    blocks.append("\nstruct VoiceLine { const uint8_t *p; uint8_t n; };\n")
    blocks.append("static const VoiceLine VOICE[] = {")
    blocks.append(",\n".join("  {VO_%s, sizeof(VO_%s)}" % (k, k) for k, _, _ in table))
    blocks.append("};\n")

    enum = []
    for i, (k, _, _) in enumerate(table):
        enum.append("  VL_%s = %d" % (k, i))
    blocks.append("enum VoiceId {\n" + ",\n".join(enum) + "\n};\n")

    OUT.write_text("\n".join(blocks), encoding="utf-8")
    print("%s  共 %d 条, 最长 %d 字节"
          % (OUT, len(table), max(n for _, _, n in table)))


if __name__ == "__main__":
    main()
