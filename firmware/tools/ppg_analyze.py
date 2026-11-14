#!/usr/bin/env python3
"""ppg_analyze.py - 把固件导出的 PPG 原始采样分析成脉率，并做链路自检

用法:
    python tools/ppg_analyze.py ppg60.csv
    python tools/ppg_analyze.py ppg60.csv --ascii 60

输入是 EdgeTerminal 用 `PPG LOG <秒>` 导出的行：
    PPG,<毫秒时间戳>,<原始计数>

这个脚本的作用不是"再算一遍脉率"，而是**给固件里的算法做独立对照**：
两边用不同的实现（固件 C++ 增量式，这里 Python 批处理）算同一条波形，
结果一致才说明固件里的峰值检测没有依赖某个巧合的初始化顺序。

同时它检查三件元数据的事，这三件出错时看波形是看不出来的：
  1. 时间戳是否单调递增（回绕或乱序会让 RR 出现负值）
  2. 采样间隔是否稳定（抖动说明主循环被别的任务卡住了）
  3. 是否有丢点（间隔超过 1.5 倍标称值）
"""

import argparse
import statistics
import sys


def load(path):
    rows = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line.startswith("PPG,"):
                continue
            parts = line.split(",")
            if len(parts) < 3:
                continue
            try:
                rows.append((int(parts[1]), float(parts[2])))
            except ValueError:
                continue
    return rows


def check_meta(rows, nominal_ms=20):
    ts = [t for t, _ in rows]
    if len(ts) < 2:
        return {"samples": len(ts)}

    mono = all(b > a for a, b in zip(ts, ts[1:]))
    gaps = [b - a for a, b in zip(ts, ts[1:])]
    dropped = sum(1 for g in gaps if g > nominal_ms * 1.5)
    return {
        "samples": len(ts),
        "span_s": (ts[-1] - ts[0]) / 1000.0,
        "monotonic": mono,
        "interval_median": statistics.median(gaps),
        "interval_max": max(gaps),
        "gaps_over_1.5x": dropped,
    }


def detect(rows, refractory_ms=None):
    """复刻固件的检测：EMA 高通 + 中点阈值 + 相对幅度门 + 自适应不应期

    相对幅度门（ppAmp >= 0.45 * 长期幅度）必须一起复刻，否则接触不良时
    两边会得出不同结论：固件判无效，这里却能算出个像样的 cv。
    """
    if not rows:
        return [], []

    dc = rows[0][1]
    ac = []
    for _, v in rows:
        dc += 0.02 * (v - dc)
        ac.append(v - dc)

    emax = emin = ac[0]
    lt = 0.0
    beats = []
    prev = ac[0]

    for i in range(1, len(rows)):
        a = ac[i]
        emax = max(a, emax * 0.995)
        emin = min(a, emin * 0.995)
        pp = emax - emin
        lt = pp if pp > lt else max(100.0, lt * 0.998)
        thr = (emax + emin) / 2.0
        amp_ok = pp >= 0.45 * lt

        ref = 300
        if len(beats) >= 4:
            rr = [beats[k + 1] - beats[k] for k in range(len(beats) - 1)]
            if rr:
                ref = max(ref, int(statistics.median(rr[-8:]) * 0.6))

        if amp_ok and prev < thr <= a and (not beats or rows[i][0] - beats[-1] >= ref):
            beats.append(rows[i][0])
        prev = a

    return beats, ac


def ascii_wave(vals, width=60):
    if not vals:
        return
    lo, hi = min(vals), max(vals)
    span = (hi - lo) or 1.0
    buckets = max(1, len(vals) // width)
    out = []
    for i in range(0, len(vals) - buckets + 1, buckets):
        chunk = vals[i:i + buckets]
        out.append("▁▂▃▄▅▆▇█"[min(7, int((sum(chunk) / len(chunk) - lo) / span * 7.999))])
    print("".join(out))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path", help="PPG LOG 导出的文本文件")
    ap.add_argument("--expect", type=float, default=None, help="固件里设定的真值 bpm，用于算误差")
    ap.add_argument("--ascii", type=int, default=0, help="波形宽度（字符数），0 表示不画")
    args = ap.parse_args()

    rows = load(args.path)
    if not rows:
        print("没读到 PPG,<ms>,<raw> 行。确认导出时固件没被 AUTO 输出打断。")
        return 1

    meta = check_meta(rows)
    print("---- 元数据 ----")
    print(f"样本数        : {meta['samples']}")
    print(f"时间跨度      : {meta['span_s']:.2f} s")
    print(f"时间戳单调    : {'是' if meta.get('monotonic') else '否  <-- 有回绕或乱序'}")
    print(f"采样间隔中位数: {meta['interval_median']:.1f} ms (标称 20)")
    print(f"最大间隔      : {meta['interval_max']} ms")
    print(f"疑似丢点次数  : {meta['gaps_over_1.5x']}")

    beats, ac = detect(rows)
    print()
    print("---- 峰值检测 ----")
    print(f"检出搏动      : {len(beats)}")

    if len(beats) >= 3:
        rr = [beats[i + 1] - beats[i] for i in range(len(beats) - 1)]
        med = statistics.median(rr)
        mean = statistics.mean(rr)
        cv = statistics.pstdev(rr) / mean if mean else 0
        bpm = 60000.0 / med
        print(f"RR 序列       : {rr}")
        print(f"中位 RR       : {med:.0f} ms")
        print(f"RR 变异系数   : {cv:.3f}")
        print(f"估计脉率      : {bpm:.1f} bpm")
        if args.expect:
            err = bpm - args.expect
            print(f"真值/误差     : {args.expect:.1f} / {err:+.1f} bpm ({err / args.expect * 100:+.1f}%)")
        if cv > 0.25:
            print("质量判定      : 差（RR 变异系数 > 0.25，固件会判为无效）")
        else:
            print("质量判定      : 可接受")
    else:
        print("搏动不足 3 次，无法算脉率（接触不良或导出时长太短）")

    if args.ascii:
        print()
        print("---- 波形 ----")
        ascii_wave([v for _, v in rows], args.ascii)

    return 0


if __name__ == "__main__":
    sys.exit(main())
