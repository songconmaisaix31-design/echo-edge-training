"""EF 复测工作簿：分层抽样、批次生成、回收校验与统计。

配套 docs/EF_REVIEW_PROTOCOL.md 与 docs/EF_ANNOTATION.md。

    python -X utf8 scripts/ef_review.py prepare  --cases work/ef-cases.json --out-dir work/ef-batches
    python -X utf8 scripts/ef_review.py check    --workbook work/ef-batches/EF-批-1.xlsx
    python -X utf8 scripts/ef_review.py analyze  --workbook-dir work/ef-batches --freeze work/ef-batches/freeze.json

病例清单、工作簿与映射表留在授权环境（work/），不入库。
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import random
import statistics
import sys
from datetime import datetime, timezone
from pathlib import Path

FIELDS = [
    ("复核编号", "review_id", 14),
    ("检查日期", "study_date", 14),
    ("判读切面", "view_code", 13),
    ("采集质量", "quality_code", 12),
    ("左室可定位", "lv_visible", 12),
    ("舒张末帧", "ed_frame", 11),
    ("收缩末帧", "es_frame", 11),
    ("测量方法", "ef_method", 12),
    ("目测 EF", "ef_visual", 11),
    ("复测 EF", "ef_value", 11),
    ("EF 是否 <50%", "ef_below_50", 13),
    ("置信度", "confidence", 10),
    ("无法判断原因", "reason", 18),
    ("复核状态", "review_status", 12),
    ("复测序号", "repeat_of", 12),
]

ENUMS = {
    "view_code": {"A4C", "PSAX", "A4C+PSAX", "UNK"},
    "quality_code": {"GOOD", "MEDIUM", "POOR"},
    "lv_visible": {"是", "否"},
    "ef_method": {"A5/6", "SIMPSON", "VISUAL"},
    "ef_below_50": {"是", "否"},
    "review_status": {"待复核", "已确认"},
}
REASONS = {"lv_unclear", "valve_unclear", "poor_quality", "view_insufficient",
           "frame_rate_low", "other"}
REQUIRED = ["review_id", "view_code", "quality_code", "lv_visible",
            "ef_method", "ef_value", "ef_below_50", "confidence", "review_status"]


def ef_bin(value: float) -> str:
    if value < 30:
        return "lt30"
    if value <= 50:
        return "30to50"
    return "gt50"


def stratified_pick(cases, n, seed):
    """按 EF 分层 × 质量分层轮转抽样，尽量让每个格子都有样本。"""
    rng = random.Random(seed)
    strata = {}
    for case in cases:
        key = (ef_bin(float(case["report_ef"])), case.get("quality_code", "UNK"))
        strata.setdefault(key, []).append(case)
    for bucket in strata.values():
        bucket.sort(key=lambda c: str(c["clip_id"]))
        rng.shuffle(bucket)
    keys = sorted(strata, key=lambda k: (k[0], k[1]))
    picked = []
    while len(picked) < n and any(strata[k] for k in keys):
        for key in keys:
            if strata[key] and len(picked) < n:
                picked.append(strata[key].pop())
    return picked


def write_workbook(path: Path, batch_id: str, rows):
    try:
        from openpyxl import Workbook
        from openpyxl.styles import Alignment, Font, PatternFill
    except ImportError as exc:  # pragma: no cover
        raise SystemExit("需要 openpyxl：python -m pip install openpyxl") from exc

    book = Workbook()
    sheet = book.active
    sheet.title = "EF复核"
    sheet.sheet_view.showGridLines = False

    sheet["A1"] = "儿童心超 EF 盲法复测"
    sheet["A1"].font = Font(name="Microsoft YaHei", size=16, bold=True, color="193245")
    sheet["A2"] = f"批次：{batch_id}　｜　判读者只按复核编号判断，不查看报告与模型输出"
    sheet["A2"].font = Font(name="Microsoft YaHei", size=10, color="536B7B")
    sheet.row_dimensions[1].height = 28
    sheet.row_dimensions[2].height = 22

    for column, (label, field, width) in enumerate(FIELDS, start=1):
        head = sheet.cell(3, column, label)
        head.font = Font(name="Microsoft YaHei", size=10, bold=True, color="FFFFFF")
        head.fill = PatternFill("solid", fgColor="244760")
        head.alignment = Alignment(horizontal="center", vertical="center")
        key = sheet.cell(4, column, field)
        key.font = Font(name="Microsoft YaHei", size=9, color="536B7B")
        key.fill = PatternFill("solid", fgColor="EAF2F7")
        sheet.column_dimensions[head.column_letter].width = width
    sheet.row_dimensions[3].height = 25

    for offset, row in enumerate(rows):
        line = 5 + offset
        sheet.cell(line, 1, row["review_id"])
        sheet.cell(line, 3, "")
        sheet.cell(line, 4, "")
        sheet.cell(line, 5, "")
        sheet.cell(line, 8, "")
        sheet.cell(line, 11, "")
        sheet.cell(line, 14, "待复核")
        sheet.cell(line, 15, row.get("repeat_of", ""))
        for column in range(1, len(FIELDS) + 1):
            sheet.cell(line, column).font = Font(name="Microsoft YaHei", size=10)
            sheet.cell(line, column).alignment = Alignment(horizontal="center")

    sheet.freeze_panes = "A5"
    path.parent.mkdir(parents=True, exist_ok=True)
    book.save(path)


def cmd_prepare(args):
    cases = json.loads(Path(args.cases).read_text(encoding="utf-8"))
    if not isinstance(cases, list) or not cases:
        raise SystemExit("病例清单必须是非空数组")
    for case in cases:
        for key in ("clip_id", "patient_id", "report_ef"):
            if key not in case:
                raise SystemExit(f"病例清单缺少字段 {key}：{case}")

    total = args.batches * args.per_batch
    if total > len(cases):
        raise SystemExit(f"需要 {total} 例，但清单只有 {len(cases)} 例")
    picked = stratified_pick(cases, total, args.seed)
    if args.repeat > len(picked):
        raise SystemExit("重测子集不能大于主样本")

    out = Path(args.out_dir)
    mapping, batches = {}, []
    index = 1
    for batch in range(1, args.batches + 1):
        rows = []
        for _ in range(args.per_batch):
            case = picked[index - 1]
            review_id = f"EF-{index:02d}"
            mapping[review_id] = {
                "clip_id": case["clip_id"],
                "patient_id": case["patient_id"],
                "report_ef": float(case["report_ef"]),
                "quality_code": case.get("quality_code", "UNK"),
                "case": case,
            }
            rows.append({"review_id": review_id})
            index += 1
        path = out / f"EF-批-{batch}.xlsx"
        write_workbook(path, f"EF-批-{batch}", rows)
        batches.append({"batch_id": f"EF-批-{batch}", "file": path.name,
                        "review_ids": [r["review_id"] for r in rows]})

    repeat_ids = sorted(mapping)[: args.repeat]
    rng = random.Random(args.seed + 1)
    rng.shuffle(repeat_ids)
    repeat_rows = []
    for offset, original in enumerate(repeat_ids):
        review_id = f"RF-{offset + 1:02d}"
        mapping[review_id] = dict(mapping[original], repeat_of=original)
        repeat_rows.append({"review_id": review_id, "repeat_of": original})
    repeat_path = out / "EF-重测.xlsx"
    write_workbook(repeat_path, "EF-重测", repeat_rows)

    digest = hashlib.sha256(json.dumps(mapping, sort_keys=True, ensure_ascii=False)
                            .encode("utf-8")).hexdigest()
    freeze = {
        "created": datetime.now(timezone.utc).isoformat(),
        "seed": args.seed,
        "batches": batches,
        "repeat": {"file": repeat_path.name,
                   "review_ids": [r["review_id"] for r in repeat_rows]},
        "mapping_sha256": digest,
        "mapping": mapping,
    }
    freeze_path = out / "freeze.json"
    freeze_path.write_text(json.dumps(freeze, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"写出 {len(batches)} 个批次 + 1 个重测批 → {out}")
    print(f"冻结文件：{freeze_path}（映射表含患者号，不得入库）")


def read_rows(path: Path):
    from openpyxl import load_workbook

    sheet = load_workbook(path, data_only=True).active
    keys = {field: column for column, (_label, field, _width) in enumerate(FIELDS, start=1)}
    rows = []
    for line in range(5, sheet.max_row + 1):
        row = {field: sheet.cell(line, column).value for field, column in keys.items()}
        if row["review_id"]:
            row["_line"] = line
            rows.append(row)
    return rows


def clean(value):
    return "" if value is None else (value.strip() if isinstance(value, str) else value)


def validate(rows):
    issues = []
    for row in rows:
        line = row["_line"]
        rid = row["review_id"]
        for field in REQUIRED:
            if clean(row.get(field)) == "":
                if field == "ef_value" and clean(row.get("lv_visible")) == "否":
                    continue
                issues.append(f"第 {line} 行 {rid}：缺少 {field}")
        for field, allowed in ENUMS.items():
            value = clean(row.get(field))
            if value not in allowed:
                issues.append(f"第 {line} 行 {rid}：{field} 取值 {value!r} 不在 {sorted(allowed)} 内")
        ef = row.get("ef_value")
        if isinstance(ef, (int, float)):
            if not 5 <= ef <= 80:
                issues.append(f"第 {line} 行 {rid}：ef_value={ef} 超出 5–80")
            expect = "是" if ef < 50 else "否"
            if clean(row.get("ef_below_50")) not in ("", expect):
                issues.append(f"第 {line} 行 {rid}：ef_value={ef} 与 ef_below_50 不一致")
        if clean(row.get("ef_method")) == "VISUAL":
            conf = row.get("confidence")
            if not isinstance(conf, (int, float)) or conf > 3:
                issues.append(f"第 {line} 行 {rid}：VISUAL 必须搭配 confidence<=3")
        if clean(row.get("lv_visible")) == "否":
            if clean(row.get("reason")) not in REASONS:
                issues.append(f"第 {line} 行 {rid}：左室不可定位时必须填写 reason")
        conf = row.get("confidence")
        if isinstance(conf, (int, float)) and conf <= 2 and clean(row.get("reason")) not in REASONS:
            issues.append(f"第 {line} 行 {rid}：confidence<=2 时必须填写 reason")
        for field in ("ed_frame", "es_frame"):
            value = row.get(field)
            if value is not None and not isinstance(value, int):
                issues.append(f"第 {line} 行 {rid}：{field} 必须是整数帧序号")
    return issues


def cmd_check(args):
    rows = read_rows(Path(args.workbook))
    issues = validate(rows)
    confirmed = sum(1 for r in rows if clean(r.get("review_status")) == "已确认")
    report = {"workbook": str(args.workbook), "rows": len(rows),
              "confirmed": confirmed, "issues": issues}
    if args.out:
        Path(args.out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.out).write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"{len(rows)} 行，已确认 {confirmed} 行，问题 {len(issues)} 条")
    for issue in issues[:args.show]:
        print(" ", issue)
    if len(issues) > args.show:
        print(f"  … 还有 {len(issues) - args.show} 条，完整列表见报告文件")
    return 0 if not issues and confirmed == len(rows) else 1


def mae(pairs):
    return statistics.fmean(abs(p - t) for p, t in pairs) if pairs else float("nan")


def agreement(pairs):
    if not pairs:
        return {}
    pred = [p for p, _t in pairs]
    true = [t for _p, t in pairs]
    diff = [p - t for p, t in pairs]
    bias = statistics.fmean(diff)
    sd = statistics.stdev(diff) if len(diff) > 1 else 0.0
    result = {"n": len(pairs), "mae": round(mae(pairs), 3), "bias": round(bias, 3),
              "loa_low": round(bias - 1.96 * sd, 3), "loa_high": round(bias + 1.96 * sd, 3)}
    if len(pairs) > 2 and statistics.pstdev(pred) > 0 and statistics.pstdev(true) > 0:
        result["pearson_r"] = round(statistics.correlation(pred, true), 4)
        result["icc_2_1"] = round(icc21(pred, true), 4)
    return result


def icc21(a, b):
    """ICC(2,1)：两方法、单次测量、绝对一致性。

    ICC(2,1) = (MSB - MSE) / (MSB + (k-1)·MSE + k·(MSR - MSE)/n)
    """
    n, k = len(a), 2
    if n < 2:
        return float("nan")
    grand = statistics.fmean(list(a) + list(b))
    ss_total = sum((v - grand) ** 2 for v in list(a) + list(b))
    ss_rows = k * sum(((ai + bi) / 2 - grand) ** 2 for ai, bi in zip(a, b))
    ss_cols = n * sum((statistics.fmean(col) - grand) ** 2 for col in (a, b))
    ss_err = ss_total - ss_rows - ss_cols
    ms_rows = ss_rows / (n - 1)
    ms_cols = ss_cols / (k - 1)
    ms_err = ss_err / ((n - 1) * (k - 1)) if (n - 1) * (k - 1) else 0.0
    denom = ms_rows + (k - 1) * ms_err + k * (ms_cols - ms_err) / n
    return (ms_rows - ms_err) / denom if denom else float("nan")


def auc(pairs):
    """EF<50 的 AUROC（Mann–Whitney）。分数是预测 EF，低 EF 才判为阳性，故按升序计胜。"""
    pos = [p for p, t in pairs if t < 50]
    neg = [p for p, t in pairs if t >= 50]
    if not pos or not neg:
        return None
    wins = sum(1.0 if p < q else 0.5 if p == q else 0.0 for p in pos for q in neg)
    return round(wins / (len(pos) * len(neg)), 4)


def cmd_analyze(args):
    freeze = json.loads(Path(args.freeze).read_text(encoding="utf-8"))
    mapping = freeze["mapping"]
    predictions = {}
    if args.predictions and Path(args.predictions).exists():
        predictions = json.loads(Path(args.predictions).read_text(encoding="utf-8"))

    rows = []
    for path in sorted(Path(args.workbook_dir).glob("*.xlsx")):
        if path.name == freeze["repeat"]["file"]:
            continue
        for row in read_rows(path):
            if clean(row.get("review_status")) != "已确认":
                continue
            rows.append(row)
    if not rows:
        raise SystemExit("没有已确认的行")

    q1, q2, triples = [], [], []
    for row in rows:
        info = mapping.get(row["review_id"])
        if not info or not isinstance(row.get("ef_value"), (int, float)):
            continue
        review_ef = float(row["ef_value"])
        report_ef = float(info["report_ef"])
        q1.append((report_ef, review_ef))
        clip_id = info["clip_id"]
        if clip_id in predictions:
            q2.append((float(predictions[clip_id]), review_ef))
        triples.append((clip_id, report_ef, review_ef))

    repeat_pairs = []
    repeat_path = Path(args.workbook_dir) / freeze["repeat"]["file"]
    if repeat_path.exists():
        by_id = {row["review_id"]: row for row in rows}
        for row in read_rows(repeat_path):
            if clean(row.get("review_status")) != "已确认":
                continue
            if not isinstance(row.get("ef_value"), (int, float)):
                continue
            original_id = mapping.get(row["review_id"], {}).get("repeat_of")
            source = by_id.get(original_id)
            if source is None or not isinstance(source.get("ef_value"), (int, float)):
                continue
            repeat_pairs.append((source["ef_value"], float(row["ef_value"])))

    result = {
        "generated": datetime.now(timezone.utc).isoformat(),
        "freeze_sha256": freeze.get("mapping_sha256"),
        "Q1_report_vs_review": agreement(q1),
        "Q2_model_vs_review": agreement(q2),
        "Q3_reviewer_repeat": agreement(repeat_pairs),
        "Q2_auroc_ef_below_50": auc(q2),
        "counts": {"confirmed_rows": len(rows), "with_prediction": len(q2),
                   "repeat_pairs": len(repeat_pairs)},
    }
    if result["Q1_report_vs_review"] and result["Q2_model_vs_review"]:
        repeat_mae = result["Q3_reviewer_repeat"].get("mae")
        result["success_criteria"] = {
            "S1_model_mae_le_report_mae":
                result["Q2_model_vs_review"]["mae"] <= result["Q1_report_vs_review"]["mae"],
            "S2_model_mae_le_repeat_mae":
                (None if repeat_mae is None
                 else result["Q2_model_vs_review"]["mae"] <= repeat_mae),
            "S3_bias_within_3":
                abs(result["Q2_model_vs_review"]["bias"]) <= 3.0,
        }
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({k: v for k, v in result.items() if k != "generated"},
                     ensure_ascii=False, indent=2))


def main():
    parser = argparse.ArgumentParser(description="EF 复测工作簿工具")
    sub = parser.add_subparsers(dest="command", required=True)

    prep = sub.add_parser("prepare", help="分层抽样并生成批次工作簿")
    prep.add_argument("--cases", required=True)
    prep.add_argument("--out-dir", required=True)
    prep.add_argument("--batches", type=int, default=6)
    prep.add_argument("--per-batch", type=int, default=20)
    prep.add_argument("--repeat", type=int, default=40)
    prep.add_argument("--seed", type=int, default=42)
    prep.set_defaults(func=cmd_prepare)

    check = sub.add_parser("check", help="校验回收的工作簿")
    check.add_argument("--workbook", required=True)
    check.add_argument("--out")
    check.add_argument("--show", type=int, default=15, help="命令行最多打印多少条问题")
    check.set_defaults(func=cmd_check)

    analyze = sub.add_parser("analyze", help="统计 Q1/Q2/Q3")
    analyze.add_argument("--workbook-dir", required=True)
    analyze.add_argument("--freeze", required=True)
    analyze.add_argument("--predictions")
    analyze.add_argument("--out", required=True)
    analyze.set_defaults(func=cmd_analyze)

    args = parser.parse_args()
    sys.exit(args.func(args) or 0)


if __name__ == "__main__":
    main()

