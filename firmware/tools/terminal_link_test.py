#!/usr/bin/env python3
"""terminal_link_test.py - 30 分钟电脑端 <-> ESP32 联动测试（issue #6）

电脑侧模拟 RTX 主机发提示，终端负责屏 + 播报 + 振动 + 回执。这个脚本负责计时、发指令、
收数据，最后给出四个可核对的数字：

  1. 提示发送数 / 终端收到数       —— 从 ACK,<seq> 里的序号比对
  2. 采样丢失数                    —— 从 ENV 行的 stamp_ms 增量推算（设备侧毫秒，不受 USB 抖动影响）
  3. 重启次数                      —— 从启动横幅出现次数判断
  4. 脉率有效率                    —— HR 行中 valid=1 的比例

用法:
    python tools/terminal_link_test.py --port COM4 --minutes 30
    python tools/terminal_link_test.py --port COM4 --minutes 1 --hint-every 5   # 冒烟测试

判据说明:
  - ENV 行每秒一条。相邻两行的 stamp_ms 差若为 2000，就是中间少了 1 秒；累加即丢样秒数。
  - ACK 的序号是设备侧递增的，缺号说明那条提示没到或被丢弃。
  - 重启判据用启动横幅，而不是"连不上"——USB 掉线也会导致读不到数据，两者不能混。
"""

import argparse
import json
import re
import sys
import time

try:
    import serial
except ImportError:
    print("需要 pyserial: pip install pyserial")
    sys.exit(2)

BOOT_MARK = "==== EdgeTerminal"
RE_ENV = re.compile(r"^ENV,([-\d.]+),([-\d.]+),([-\d.]+),(\d),(\d),(\d),(\d+),(\d+)$")
RE_HR = re.compile(r"^HR,([-\d.]+),(\d+),(\d),(\d),(\d+)$")
RE_ACK = re.compile(r"^ACK,(\d+)$")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM4")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--minutes", type=float, default=30)
    ap.add_argument("--hint-every", type=float, default=20, help="每秒发一条提示的间隔")
    ap.add_argument("--out", default="logs/link_report.json")
    args = ap.parse_args()

    duration = args.minutes * 60

    def fresh_state():
        return {
            "hints_sent": 0,
            "acks": [],
            "env_lines": 0,
            "env_first_stamp": None,
            "env_last_stamp": None,
            "env_gap_slots": 0,
            "env_max_gap_ms": 0,
            "hr_lines": 0,
            "hr_valid": 0,
            "reboots": 0,
            "invalid_lines": 0,
            "bytes": 0,
        }

    st = fresh_state()

    print(f"[LINK] {args.port} @{args.baud}, 时长 {args.minutes} 分钟, 每 {args.hint_every}s 一条提示")
    print("[LINK] 提示先发 3 条当作预热，不计入统计" if duration > 120 else "")

    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    ser.setDTR(False)
    ser.setRTS(False)
    time.sleep(0.3)

    # 预热：让固件跑完启动流程
    time.sleep(2.0)
    ser.reset_input_buffer()

    # 预热提示单独发、发完把统计整个清空。
    # 踩过：把预热提示算进"已发"但回执全算进"到达"，会得到 103% 这种数。
    if duration > 120:
        print("[LINK] 预热 3 条（完成后统计清零）")
        for _ in range(3):
            ser.write(b"HINT 1\r")
            time.sleep(1.0)
        time.sleep(2.0)
        ser.reset_input_buffer()
        st = fresh_state()

    t0 = time.time()
    next_hint = time.time()
    hint_no = 1
    buf = b""

    try:
        while time.time() - t0 < duration:
            now = time.time()

            if now >= next_hint:
                n = ((hint_no - 1) % 8) + 1
                ser.write(b"HINT %d\r" % n)
                st["hints_sent"] += 1
                hint_no += 1
                next_hint = now + args.hint_every

            chunk = ser.read(4096)
            if not chunk:
                continue
            st["bytes"] += len(chunk)
            buf += chunk

            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").strip()
                if not line:
                    continue

                if BOOT_MARK in line:
                    st["reboots"] += 1
                    continue

                m = RE_ENV.match(line)
                if m:
                    stamp = int(m.group(7))
                    st["env_lines"] += 1
                    if st["env_first_stamp"] is None:
                        st["env_first_stamp"] = stamp
                    if st["env_last_stamp"] is not None:
                        d = stamp - st["env_last_stamp"]
                        if d > st["env_max_gap_ms"]:
                            st["env_max_gap_ms"] = d
                        if d > 1500:
                            st["env_gap_slots"] += max(0, round(d / 1000.0) - 1)
                    st["env_last_stamp"] = stamp
                    continue

                m = RE_HR.match(line)
                if m:
                    st["hr_lines"] += 1
                    if m.group(3) == "1":
                        st["hr_valid"] += 1
                    continue

                m = RE_ACK.match(line)
                if m:
                    st["acks"].append(int(m.group(1)))
                    continue

                if line.startswith("ENV,") or line.startswith("HR,"):
                    st["invalid_lines"] += 1
    except KeyboardInterrupt:
        print("\n[LINK] 手动中断，按已收数据分析")
    finally:
        ser.close()

    elapsed = time.time() - t0
    expected_s = int(elapsed)
    acks = st["acks"]
    ack_first = acks[0] if acks else 0
    ack_last = acks[-1] if acks else 0
    ack_expected = (ack_last - ack_first + 1) if acks else 0
    ack_got = len(set(acks))

    # 启动横幅在预热阶段就被排空了，之后每出现一次就是真重启，不需要再减 1
    report = {
        "port": args.port,
        "duration_s": round(elapsed, 1),
        "hints_sent": st["hints_sent"],
        "acks_received": ack_got,
        "ack_seq_first_last": [ack_first, ack_last],
        "ack_missing": max(0, ack_expected - ack_got),
        "ack_arrival_pct": round(100.0 * ack_got / st["hints_sent"], 2) if st["hints_sent"] else None,
        "env_lines": st["env_lines"],
        "env_expected": expected_s,
        "env_loss_slots": st["env_gap_slots"],
        "env_loss_pct": round(100.0 * st["env_gap_slots"] / expected_s, 2) if expected_s else None,
        "env_max_gap_ms": st["env_max_gap_ms"],
        "env_malformed": st["invalid_lines"],
        "hr_lines": st["hr_lines"],
        "hr_valid_pct": round(100.0 * st["hr_valid"] / st["hr_lines"], 1) if st["hr_lines"] else None,
        "reboots": st["reboots"],
        "bytes": st["bytes"],
    }

    print("\n================ 结果 ================")
    print(f"时长              {report['duration_s']} s")
    print(f"提示 发送/到达    {report['hints_sent']} / {report['acks_received']}"
          f"  (到达率 {report['ack_arrival_pct']}%, 缺号 {report['ack_missing']})")
    print(f"采样 实收/应得    {report['env_lines']} / {report['env_expected']}"
          f"  (丢 {report['env_loss_slots']} 秒, {report['env_loss_pct']}%)")
    print(f"相邻 ENV 最大间隔 {report['env_max_gap_ms']} ms")
    print(f"脉率 行/有效      {report['hr_lines']} / {report['hr_valid_pct']}%")
    print(f"重启次数          {report['reboots']}")
    print(f"格式错误行        {report['env_malformed']}")
    print("=====================================")

    ok = (report["ack_arrival_pct"] == 100.0 and report["reboots"] == 0
          and report["env_loss_pct"] is not None and report["env_loss_pct"] <= 1.0)
    print("\n判定: " + ("通过 (提示全到达, 无重启, 丢样 <= 1%)" if ok else "未通过, 逐项看上面"))
    print("提醒: 这份结果证明的是链路与调度, 屏/播报/震动的物理效果仍需现场核对。")

    try:
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(report, f, ensure_ascii=False, indent=2)
        print(f"已写入 {args.out}")
    except OSError as e:
        print(f"写报告失败: {e}")

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
