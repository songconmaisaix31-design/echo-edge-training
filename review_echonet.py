import argparse
import json
import math
import random
import re
from bisect import bisect_left
from collections import defaultdict
from pathlib import Path
from zipfile import ZipFile

from openpyxl import load_workbook

from train_echo import video_frames
from train_echonet_views import SOURCE_SPLITS, VIEWS, open_avi, read_sources


REVIEW_HEADERS = (
    "review_id", "start_ms", "end_ms", "view", "psax_level", "quality",
    "structure_notes", "reason", "review_status",
)
REVIEW_VIEWS = {*VIEWS, "其他", "过渡", "无法判断"}
REVIEW_QUALITIES = {"可评估", "需补扫", "无法评估"}
REVIEW_STATUSES = {"待复核", "已确认"}


def require_distinct_paths(*paths):
    if len({path.resolve() for path in paths}) != len(paths):
        raise ValueError("输入和输出必须使用不同文件")


def write_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def select_clips(archive, seed):
    errors = []
    clips = read_sources(archive, errors)
    if errors:
        raise ValueError(f"归档含 {len(errors)} 处索引错误，首处为 {errors[0]}")
    groups = defaultdict(list)
    for clip in clips:
        _, view, split, _, _ = clip
        groups[view, split].append(clip)
    rng = random.Random(seed)
    selected = []
    for view in VIEWS:
        for split in SOURCE_SPLITS:
            group = groups[view, split]
            if not group:
                raise ValueError(f"缺少 {view} 切面、原始 Split {split}")
            selected.append(rng.choice(sorted(group)))
    rng.shuffle(selected)
    return selected, len(clips)


def inspect_clip(archive, clip, review_id):
    source_path, source_view, split, _, _ = clip
    offsets = []
    frame_duration = 0
    with open_avi(archive, source_path) as container:
        stream = container.streams.video[0]
        stream.codec_context.thread_count = 1
        for offset, frame in video_frames(container, stream):
            if not math.isfinite(offset):
                raise ValueError(f"{review_id} 有无效的呈现时间戳")
            offsets.append(offset)
            frame_duration = float(frame.duration * frame.time_base * 1000) if frame.duration else 0
    if not offsets:
        raise ValueError(f"{review_id} 没有可解码视频帧")
    last = offsets[-1]
    return {
        "review_id": review_id,
        "source_view": source_view,
        "split": split,
        "source_path": source_path,
        "frames": len(offsets),
        "last_pts_ms": last,
        "duration_ms": max(math.floor(last) + 1, math.ceil(last + frame_duration - 1e-6)),
        "frame_pts_ms": offsets,
    }


def prepare(args):
    require_distinct_paths(args.archive, args.index, args.review_input)
    batch_id = args.batch_id or f"echonet-review-{args.seed}"
    if not re.fullmatch(r"[a-zA-Z0-9][a-zA-Z0-9_.-]{0,79}", batch_id):
        raise ValueError("批次编号只接受字母、数字、点、短横线和下划线")
    with ZipFile(args.archive, metadata_encoding="utf-8") as archive:
        selected, source_count = select_clips(archive, args.seed)
        rows = [inspect_clip(archive, clip, f"REV-{number:02d}") for number, clip in enumerate(selected, start=1)]
    write_json(args.index, {"batch_id": batch_id, "seed": args.seed, "source_count": source_count, "rows": rows})
    review_rows = [{key: row[key] for key in ("review_id", "duration_ms")} for row in rows]
    write_json(args.review_input, {"batch_id": batch_id, "rows": review_rows})
    print(f"批次 {batch_id} 已抽取并检查 {len(rows)} 段，共 {sum(row['frames'] for row in rows)} 个源帧。")
    return 0


def whole_milliseconds(value):
    return (isinstance(value, (int, float)) and not isinstance(value, bool)
            and math.isfinite(value) and int(value) == value)


def interval_issues(start, end, clip):
    if start is None and end is None:
        return []
    if not whole_milliseconds(start) or not whole_milliseconds(end):
        return ["开始和结束时间须同时填写整数毫秒，整段复核时两格留空"]
    if not 0 <= start < end <= clip["duration_ms"]:
        return ["区间须满足 0 ≤ 开始 < 结束 ≤ 本段时长"]
    offsets = clip["frame_pts_ms"]
    first = bisect_left(offsets, start)
    if first == len(offsets) or offsets[first] >= end:
        return ["该半开区间内没有源视频帧"]
    return []


def choices_issue(value, options, name):
    if value is not None and value not in options:
        return [f"{name}不在模板允许的选项中"]
    return []


def inspect_review_sheet(sheet, rows, batch_id):
    errors = []
    seen = set()
    confirmed, pending, quality_unrated = [], [], []
    if sheet["A2"].value != f"批次：{batch_id}":
        errors.append({"row": 2, "message": "批次编号与本地索引不一致"})
    headers = next(sheet.iter_rows(min_row=4, max_row=4, max_col=len(REVIEW_HEADERS), values_only=True), ())
    if headers != REVIEW_HEADERS:
        errors.append({"row": 4, "message": "表头与医生复核模板不一致"})
        return errors, confirmed, sorted(rows), quality_unrated
    for line, cells in enumerate(sheet.iter_rows(min_row=5, max_col=len(REVIEW_HEADERS)), start=5):
        values = [cell.value for cell in cells]
        if all(value is None for value in values):
            continue
        if any(cell.data_type == "f" for cell in cells):
            errors.append({"row": line, "message": "复核单元格须填写实际值"})
            continue
        review_id, start, end, view, level, quality, _, reason, status = values
        if not isinstance(review_id, str) or review_id not in rows:
            errors.append({"row": line, "message": "复核编号未在当前批次登记"})
            continue
        if review_id in seen:
            errors.append({"row": line, "review_id": review_id, "message": "同一复核编号重复出现"})
            continue
        seen.add(review_id)
        issues = interval_issues(start, end, rows[review_id])
        issues += choices_issue(view, REVIEW_VIEWS, "切面")
        issues += choices_issue(quality, REVIEW_QUALITIES, "质量")
        issues += choices_issue(status, REVIEW_STATUSES, "复核状态")
        if level is not None and view != "PSAX":
            issues.append("仅 PSAX 填写短轴层面")
        if status == "已确认" and view is None and quality is None:
            issues.append("已确认行至少填写切面或采集质量")
        if status == "已确认" and quality in {"需补扫", "无法评估"} and not (isinstance(reason, str) and reason.strip()):
            issues.append("需补扫或无法评估时须填写原因")
        if issues:
            errors.extend({"row": line, "review_id": review_id, "message": issue} for issue in issues)
        elif status == "已确认":
            confirmed.append(review_id)
            if quality is None:
                quality_unrated.append(review_id)
        else:
            pending.append(review_id)
    for missing in sorted(rows.keys() - seen):
        errors.append({"review_id": missing, "message": "复核表缺少该编号"})
    return errors, confirmed, pending, quality_unrated


def check(args):
    require_distinct_paths(args.index, args.workbook, args.report)
    index = json.loads(args.index.read_text(encoding="utf-8"))
    rows = {row["review_id"]: row for row in index["rows"]}
    workbook = load_workbook(args.workbook, read_only=True, data_only=False, keep_links=False)
    try:
        if "医生复核" not in workbook.sheetnames:
            raise ValueError("缺少工作表：医生复核")
        errors, confirmed, pending, quality_unrated = inspect_review_sheet(workbook["医生复核"], rows, index["batch_id"])
    finally:
        workbook.close()
    state = "待修正" if errors else "待医生复核" if pending else "已完成复核"
    write_json(args.report, {
        "batch_id": index["batch_id"], "status": state,
        "total": len(rows), "confirmed_count": len(confirmed), "pending_count": len(pending),
        "confirmed": confirmed, "pending": pending, "confirmed_quality_unrated": quality_unrated, "errors": errors,
    })
    print(f"{state}，已确认 {len(confirmed)}/{len(rows)} 段，发现 {len(errors)} 项表格问题。")
    return 1 if errors else 0


def locate(args):
    index = json.loads(args.index.read_text(encoding="utf-8"))
    row = next((row for row in index["rows"] if row["review_id"] == args.review_id), None)
    if row is None:
        raise ValueError(f"{args.review_id} 未在该批次登记")
    print(json.dumps({key: row[key] for key in ("review_id", "source_path", "duration_ms")},
                     ensure_ascii=False, indent=2))
    return 0


def main():
    parser = argparse.ArgumentParser(description="EchoNet-Pediatric 20 段切面与质量人工复核准备")
    commands = parser.add_subparsers(dest="command", required=True)
    sampling = commands.add_parser("prepare", help="分层抽样并验证源视频，生成本地索引及复核表输入")
    sampling.add_argument("--archive", type=Path, required=True)
    sampling.add_argument("--index", type=Path, required=True, help="本地私有 JSON 索引")
    sampling.add_argument("--review-input", type=Path, required=True, help="仅含复核编号和时长的 JSON")
    sampling.add_argument("--seed", type=int, default=42)
    sampling.add_argument("--batch-id")
    checking = commands.add_parser("check", help="检查医生填写的复核表")
    checking.add_argument("--index", type=Path, required=True)
    checking.add_argument("--workbook", type=Path, required=True)
    checking.add_argument("--report", type=Path, required=True)
    locating = commands.add_parser("locate", help="供本地操作者查找原始 ZIP 内路径")
    locating.add_argument("--index", type=Path, required=True)
    locating.add_argument("--review-id", required=True)
    args = parser.parse_args()
    if args.command == "prepare":
        return prepare(args)
    if args.command == "locate":
        return locate(args)
    return check(args)


if __name__ == "__main__":
    raise SystemExit(main())
