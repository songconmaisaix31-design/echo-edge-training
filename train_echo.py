import argparse
import json
import math
import random
import shutil
import tempfile
from bisect import bisect_left
from collections import Counter, defaultdict
from contextlib import closing, contextmanager, nullcontext
from dataclasses import dataclass
from decimal import Decimal
from pathlib import Path, PurePosixPath
from zipfile import BadZipFile, ZipFile
from xml.etree.ElementTree import ParseError

import av
import torch
from PIL import Image
from av.video.reformatter import VideoReformatter
from openpyxl import load_workbook
from openpyxl.utils.exceptions import InvalidFileException
from pydicom import dcmread
from pydicom.errors import InvalidDicomError
from pydicom.misc import is_dicom
from torch import nn
from torch.nn import functional as F
from torch.utils.data import DataLoader, Dataset
from torchvision.models import MobileNet_V3_Small_Weights, mobilenet_v3_small

from benchmark_echo import configure_video, normalize_image, preprocess, resize_and_pad
from echo_dicom import DicomCine, US_CINE_SOP_CLASSES, color_counts


WEIGHTS = MobileNet_V3_Small_Weights.IMAGENET1K_V1
TASKS = ("view", "quality")
IGNORE = -100
VIDEO_SUFFIXES = {".mp4", ".mov"}
DICOM_SUFFIXES = {".dcm", ".dicom", ".ima", ""}
MEDIA_SUFFIXES = VIDEO_SUFFIXES | DICOM_SUFFIXES
HEADERS = {
    "片段标注": ("segment_id", "clip_id", "start_ms", "end_ms", "view_code", "quality_code",
                 "quality_reason", "structure_notes", "annotator_id", "review_status", "segment_mode"),
    "视频清单": ("clip_id", "patient_id", "study_id", "relative_path", "mode", "duration_ms",
                 "age_band", "site_code", "device_model", "probe_model", "split"),
    "标签字典": ("task", "label_code", "display_name", "definition", "mode_scope", "required_structures"),
}


class EchoModel(nn.Module):
    def __init__(self, view_count, quality_count):
        super().__init__()
        backbone = mobilenet_v3_small(weights=WEIGHTS)
        self.features = backbone.features
        self.pool = backbone.avgpool
        width = backbone.classifier[0].in_features
        self.view_head = nn.Linear(width, view_count)
        self.quality_head = nn.Linear(width, quality_count)

    def forward(self, images):
        features = self.pool(self.features(images)).flatten(1)
        return self.view_head(features), self.quality_head(features)


@dataclass(frozen=True)
class Clip:
    path: str
    patient: str
    split: str
    duration_ms: int | None
    row: int = 0


@dataclass(frozen=True)
class Segment:
    key: str
    clip_id: str
    start_ms: int
    end_ms: int
    targets: tuple[int, int]
    row: int = 0


@dataclass(frozen=True)
class TrainingInputs:
    labels: dict
    clips: dict[str, Clip]
    segments: dict[str, list[Segment]]
    assignments: dict[str, str]


def workbook_rows(path, issue=None):
    book = load_workbook(path, read_only=True, data_only=False)
    try:
        tables = {}
        for name, headers in HEADERS.items():
            if name not in book:
                if issue is not None:
                    issue("missing_sheet", f"缺少工作表 {name}", sheet=name)
                    tables[name] = []
                    continue
                raise ValueError(f"缺少工作表 {name}")
            rows = book[name].iter_rows(min_row=4, max_col=len(headers))
            first = next(rows, ())
            if tuple(cell.value for cell in first) != headers:
                if issue is not None:
                    issue("invalid_header", "列名与模板不一致", sheet=name, row=4)
                    tables[name] = []
                    continue
                raise ValueError(f"{name} 第 4 行列名与模板不一致")
            tables[name] = []
            for number, cells in enumerate(rows, start=5):
                if all(cell.value is None for cell in cells):
                    continue
                if any(cell.data_type == "f" for cell in cells):
                    if issue is not None:
                        issue("formula_in_input", "请填实际值", sheet=name, row=number)
                        continue
                    raise ValueError(f"{name} 第 {number} 行包含公式，请填实际值")
                values = dict(zip(headers, (cell.value for cell in cells), strict=True))
                values["_row"] = number
                tables[name].append((f"{name} 第 {number} 行", values))
        return tables
    finally:
        book.close()


def string(row, key, where, required=True):
    value = row[key]
    if (value is None or value == "") and not required:
        return ""
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"{where} 的 {key} 需要填写文本")
    return value.strip()


def milliseconds(row, key, where, required=True):
    value = row[key]
    if value is None and not required:
        return None
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or int(value) != value or value < 0:
        raise ValueError(f"{where} 的 {key} 需要填写非负整数毫秒")
    return int(value)


def dictionary_entry(row, where):
    task = string(row, "task", where)
    if task not in TASKS:
        raise ValueError(f"{where} 的 task 可填写 view 或 quality")
    code = string(row, "label_code", where)
    return task, code, {"name": string(row, "display_name", where),
                        "definition": string(row, "definition", where)}

def clip_entry(row, where):
    clip_id = string(row, "clip_id", where)
    study = string(row, "study_id", where)
    patient = string(row, "patient_id", where)
    path = string(row, "relative_path", where).replace("\\", "/")
    relative = PurePosixPath(path)
    split = string(row, "split", where, required=False)
    string(row, "mode", where)
    if (not relative.name or relative.is_absolute() or ".." in relative.parts or ":" in path
            or relative.suffix.lower() not in MEDIA_SUFFIXES or relative.name.upper() == "DICOMDIR"):
        raise ValueError(f"{where} 的 relative_path 需要指向 DICOM cine、MP4 或 MOV 相对路径")
    if split not in {"", "train", "val", "test"}:
        raise ValueError(f"{where} 的 split 可填写 train、val 或 test")
    duration = milliseconds(row, "duration_ms", where, required=False)
    if duration == 0:
        raise ValueError(f"{where} 的视频时长必须大于零")
    return clip_id, study, Clip(relative.as_posix(), patient, split, duration, row["_row"])


def review_status(row, where):
    status = string(row, "review_status", where, required=False)
    if status not in {"", "待复核", "已确认"}:
        raise ValueError(f"{where} 的 review_status 可填写待复核或已确认")
    return status


def segment_interval(row, where, clips):
    key = string(row, "segment_id", where)
    clip_id = string(row, "clip_id", where)
    start = milliseconds(row, "start_ms", where)
    end = milliseconds(row, "end_ms", where)
    if clip_id not in clips:
        raise ValueError(f"{where} 的 clip_id 未在有效的视频清单行中登记")
    if end <= start:
        raise ValueError(f"{where} 的 end_ms 必须大于 start_ms")
    if clips[clip_id].duration_ms is not None and end > clips[clip_id].duration_ms:
        raise ValueError(f"{where} 的结束时间超过视频清单中的时长")
    return key, clip_id, start, end


def segment_targets(row, where, label_indexes):
    targets = []
    for task in TASKS:
        code = string(row, f"{task}_code", where, required=False)
        if code and code not in label_indexes[task]:
            raise ValueError(f"{where} 的 {task}_code 未在字典中定义：{code}")
        targets.append(label_indexes[task][code] if code else IGNORE)
    if all(target == IGNORE for target in targets):
        raise ValueError(f"{where} 至少需要一个已确认标签")
    return tuple(targets)


def read_annotations(path, issue=None):
    tables = workbook_rows(path, issue)
    labels = {task: {} for task in TASKS}
    clips, paths, studies, keys = {}, {}, {}, {}
    dictionary_rows = {task: {} for task in TASKS}
    segments = defaultdict(list)
    confirmed = defaultdict(list)
    pending = []
    registered = []
    checkable = []

    def record(code, error, sheet, row, **details):
        if issue is None:
            raise error
        issue(code, str(error), sheet=sheet, row=row, **details)

    for where, row in tables["标签字典"]:
        try:
            task, code, entry = dictionary_entry(row, where)
            if code in labels[task]:
                raise ValueError(f"{where} 的 {task}/{code} 与第 {dictionary_rows[task][code]} 行重复")
            labels[task][code] = entry
            dictionary_rows[task][code] = row["_row"]
        except ValueError as error:
            record("invalid_dictionary_entry", error, "标签字典", row["_row"])
    for task, entries in labels.items():
        if len(entries) < 2:
            record("too_few_labels", ValueError(f"标签字典中的 {task} 需要至少两种有效标签"), "标签字典", 4)

    for where, row in tables["视频清单"]:
        try:
            clip_id, study, clip = clip_entry(row, where)
            registered.append((clip_id, clip))
            if clip_id in clips:
                raise ValueError(f"{where} 的 clip_id 与第 {clips[clip_id].row} 行重复")
            if clip.path.casefold() in paths:
                raise ValueError(f"{where} 的路径与第 {paths[clip.path.casefold()]} 行重复")
            if study in studies and studies[study][0] != clip.patient:
                raise ValueError(f"{where} 的 study_id 与第 {studies[study][1]} 行关联到不同患儿")
            clips[clip_id] = clip
            paths[clip.path.casefold()] = clip.row
            studies[study] = (clip.patient, clip.row)
            checkable.append((clip_id, clip))
        except ValueError as error:
            record("invalid_clip_entry", error, "视频清单", row["_row"], file=row["relative_path"], clip_id=row["clip_id"])
            if issue is not None and isinstance(row["relative_path"], str):
                # 分组或其他列错误不应阻止定位已填写的文件。
                candidate = PurePosixPath(row["relative_path"].strip().replace("\\", "/"))
                if candidate.name and not candidate.is_absolute() and ".." not in candidate.parts and ":" not in str(candidate):
                    file_id = row["clip_id"] if isinstance(row["clip_id"], str) else ""
                    checkable.append((file_id.strip(), Clip(candidate.as_posix(), "", "", None, row["_row"])))

    label_indexes = {task: {code: index for index, code in enumerate(labels[task])} for task in TASKS}
    for where, row in tables["片段标注"]:
        clip_value = row["clip_id"]
        clip = clips.get(clip_value.strip()) if isinstance(clip_value, str) else None
        details = {"file": clip.path if clip else None, "clip_id": clip_value, "segment_id": row["segment_id"]}
        try:
            status = review_status(row, where)
            if status != "已确认":
                pending.append(row["_row"])
                continue
            key, clip_id, start, end = segment_interval(row, where, clips)
            if key in keys:
                raise ValueError(f"{where} 的 segment_id 与第 {keys[key]} 行重复")
            keys[key] = row["_row"]
        except ValueError as error:
            record("invalid_segment", error, "片段标注", row["_row"], **details)
            continue
        confirmed[clip_id].append(Segment(key, clip_id, start, end, (IGNORE, IGNORE), row["_row"]))
        try:
            targets = segment_targets(row, where, label_indexes)
            segments[clip_id].append(Segment(key, clip_id, start, end, targets, row["_row"]))
        except ValueError as error:
            record("invalid_segment_label", error, "片段标注", row["_row"], **details)
    if not segments:
        record("no_confirmed_labels", ValueError("片段标注中没有有效的已确认标注"), "片段标注", 4)
    for clip_id, rows in confirmed.items():
        rows.sort(key=lambda segment: segment.start_ms)
        latest = rows[0]
        for right in rows[1:]:
            if latest.end_ms > right.start_ms:
                record("overlapping_intervals", ValueError(f"已确认区间与片段标注第 {latest.row} 行重叠"),
                       "片段标注", right.row, file=clips[clip_id].path, clip_id=clip_id,
                       segment_id=right.key, related_rows=[latest.row])
            if right.end_ms > latest.end_ms:
                latest = right
    for rows in segments.values():
        rows.sort(key=lambda segment: segment.start_ms)
    if issue is not None:
        return labels, clips, segments, confirmed, pending, registered, dictionary_rows, checkable
    return labels, clips, segments


def patient_splits(clips, segments, labels, seed, val_fraction, coverage_issue=None):
    assignments = {}
    for clip in clips.values():
        if clip.split:
            if assignments.get(clip.patient, clip.split) != clip.split:
                raise ValueError(f"患儿 {clip.patient} 同时出现在多个数据分组")
            assignments[clip.patient] = clip.split
    by_patient = defaultdict(set)
    for clip_id, rows in segments.items():
        supported = by_patient[clips[clip_id].patient]
        supported.update((task, target) for row in rows for task, target in zip(TASKS, row.targets) if target != IGNORE)
    if assignments:
        if set(by_patient) - assignments.keys():
            raise ValueError("使用手工 split 时，请为所有参与训练的患儿指定分组")
    else:
        patients = sorted(by_patient)
        random.Random(seed).shuffle(patients)
        support = Counter(label for values in by_patient.values() for label in values)
        assignments = dict.fromkeys(patients, "train")
        wanted = max(1, round(len(patients) * val_fraction))
        validation_tasks = set()
        moved = 0
        while moved < wanted or validation_tasks != set(TASKS):
            eligible = [patient for patient in patients if assignments[patient] == "train"
                        and all(support[label] > 1 for label in by_patient[patient])]
            if not eligible:
                break
            patient = max(eligible, key=lambda candidate: len(
                {task for task, _ in by_patient[candidate]} - validation_tasks,
            ))
            assignments[patient] = "val"
            support.subtract(by_patient[patient])
            validation_tasks.update(task for task, _ in by_patient[patient])
            moved += 1
    observed = defaultdict(set)
    for patient, values in by_patient.items():
        observed[assignments[patient]].update(values)
    required = {(task, index) for task in TASKS for index in range(len(labels[task]))}
    if required - observed["train"]:
        missing = [(task, list(labels[task])[index]) for task, index in sorted(required - observed["train"])]
        absent = ", ".join(f"{task}/{code}" for task, code in missing)
        message = f"训练组缺少标签 {absent}，请补充标注或调整患儿分组"
        if coverage_issue is None:
            raise ValueError(message)
        coverage_issue("train_label_missing", message, missing=missing)
    if {task for task, _ in observed["val"]} != set(TASKS):
        message = "验证组需要来自独立患儿，并且切面和质量两个任务均有标注"
        if coverage_issue is None:
            raise ValueError(message)
        coverage_issue("validation_task_missing", message)
    return assignments


def open_archive(media):
    if media.is_dir():
        return nullcontext(None)
    if media.is_file() and media.suffix.lower() == ".zip":
        return ZipFile(media, metadata_encoding="utf-8")
    if media.is_file() and media.suffix.lower() in MEDIA_SUFFIXES:
        return nullcontext(None)
    raise ValueError("--media 需要指向 DICOM cine、MP4/MOV 文件、影像目录或 ZIP 压缩包")


@contextmanager
def local_media(media, archive, name, scratch):
    if archive is None:
        root = (media if media.is_dir() else media.parent).resolve()
        path = (root / name).resolve()
        if (not path.is_relative_to(root) or not path.is_file()
                or (media.is_file() and path != media.resolve())):
            raise ValueError(f"影像来源中找不到 {name}")
        yield path
    else:
        with tempfile.TemporaryDirectory(dir=scratch) as staging:
            path = Path(staging) / Path(name).name
            with archive.open(name) as source, path.open("wb") as target:
                shutil.copyfileobj(source, target, 1024 * 1024)
            yield path


def video_frames(container, stream):
    origin = None
    previous = -math.inf
    for frame in container.decode(stream):
        timestamp = frame.time
        if timestamp is None or timestamp < previous:
            raise ValueError("源视频缺少有效且单调的呈现时间戳")
        previous = timestamp
        if origin is None:
            origin = timestamp
        yield (timestamp - origin) * 1000, frame


def sampling_slot(offset_ms, rate):
    return math.floor(max(0, offset_ms) * rate / 1000 + 1e-6)


def dicom_samples_at_rate(timed_items, rate):
    frames_per_second = Decimal(str(rate))
    last_slot = -1
    for offset_ms, item in timed_items:
        slot = int(offset_ms * frames_per_second // 1000)
        if slot != last_slot:
            last_slot = slot
            yield offset_ms, item


def samples_in_intervals(timed_items, rows, sample_bins, exact_timing=False):
    index = 0
    last_bin = -1
    tolerance = 0 if exact_timing else 1e-6
    for offset_ms, item in timed_items:
        while index < len(rows) and offset_ms + tolerance >= rows[index].end_ms:
            index += 1
            last_bin = -1
        if index == len(rows):
            break
        row = rows[index]
        if offset_ms + tolerance < row.start_ms:
            continue
        duration = row.end_ms - row.start_ms
        if exact_timing:
            time_bin = int((offset_ms - row.start_ms) * sample_bins[index] // duration)
        else:
            rate = 1000 * sample_bins[index] / duration
            time_bin = min(sample_bins[index] - 1, sampling_slot(offset_ms - row.start_ms, rate))
        if time_bin != last_bin:
            last_bin = time_bin
            yield index, item


def check_duration(path, rows, duration_ms):
    if duration_ms is not None and rows[-1].end_ms > math.ceil(duration_ms):
        raise ValueError(f"{path.name} 的标注超出影像时长")


def selected_interval_frames(path, rows, sample_bins):
    if path.suffix.lower() in VIDEO_SUFFIXES:
        with av.open(str(path)) as container:
            stream = configure_video(container)
            duration_ms = None if stream.duration is None else float(stream.duration * stream.time_base) * 1000
            check_duration(path, rows, duration_ms)
            yield from samples_in_intervals(video_frames(container, stream), rows, sample_bins)
    else:
        cine = DicomCine(path)
        check_duration(path, rows, cine.duration_ms)
        picks = list(samples_in_intervals(cine.timed_indices(), rows, sample_bins, exact_timing=True))
        frames = cine.decode([frame_index for _, frame_index in picks])
        for (segment_index, _), frame in zip(picks, frames, strict=True):
            yield segment_index, frame


def interval_bins(row, args):
    return min(args.max_frames_per_segment, max(1, math.ceil(
        (row.end_ms - row.start_ms) * args.sample_fps / 1000,
    )))


class FrameDataset(Dataset):
    def __init__(self, rows, normalization):
        self.rows = rows
        self.normalization = normalization

    def __len__(self):
        return len(self.rows)

    def __getitem__(self, index):
        path, targets = self.rows[index]
        with Image.open(path) as image:
            return normalize_image(image, self.normalization), torch.tensor(targets, dtype=torch.long)


def prepare_frames(args, clips, segments, assignments, scratch):
    records = defaultdict(list)
    with open_archive(args.media) as archive:
        for clip_index, (clip_id, rows) in enumerate(segments.items()):
            clip = clips[clip_id]
            split = assignments[clip.patient]
            if split == "test":
                continue
            sample_bins = [interval_bins(row, args) for row in rows]
            counts = [0] * len(rows)
            scaler = VideoReformatter()
            with local_media(args.media, archive, clip.path, scratch) as path:
                for index, frame in selected_interval_frames(path, rows, sample_bins):
                    target = scratch / f"{clip_index}_{index}_{counts[index]}.png"
                    resize_and_pad(frame, scaler).save(target, compress_level=1)
                    records[split].append((target, rows[index].targets))
                    counts[index] += 1
            missing = [row.key for row, count in zip(rows, counts, strict=True) if count == 0]
            if missing:
                raise ValueError(f"视频 {clip_id} 的标注片段没有抽到真实帧：{', '.join(missing)}")
    return records


MEDIA_INPUT_ERRORS = (ValueError, OSError, KeyError, IndexError, AttributeError, RuntimeError,
                      NotImplementedError, BadZipFile, InvalidDicomError, ArithmeticError, av.FFmpegError)


def preflight_media(args, checkable, clips, confirmed, issue):
    files, checked = [], []
    try:
        archive_context = open_archive(args.media)
    except (ValueError, OSError, BadZipFile) as error:
        issue("invalid_media_source", str(error), file=str(args.media))
        return files, checked

    with archive_context as archive, tempfile.TemporaryDirectory(dir=args.cache_dir) as scratch:
        names = Counter(archive.namelist()) if archive is not None else Counter()
        for clip_id, clip in checkable:
            locations = {"file": clip.path, "clip_id": clip_id}
            rows = confirmed.get(clip_id, ()) if clips.get(clip_id) == clip else ()
            item = {"sheet": "视频清单", "row": clip.row, **locations,
                    "exists": names[clip.path] > 0, "readable": False, "annotated_rows": [row.row for row in rows]}
            files.append(item)
            try:
                if names[clip.path] > 1:
                    issue("ambiguous_zip_path", "ZIP 内同一路径出现多次", sheet="视频清单", row=clip.row, **locations)
                    continue
                with local_media(args.media, archive, clip.path, scratch) as path:
                    item["exists"] = True
                    if path.suffix.lower() in VIDEO_SUFFIXES:
                        with av.open(str(path)) as container:
                            stream = configure_video(container)
                            duration = None if stream.duration is None else float(stream.duration * stream.time_base) * 1000
                            offsets = [offset for offset, _ in video_frames(container, stream)]
                            if not offsets:
                                raise ValueError("视频没有可解码的源帧")
                        cine = None
                        item["timing_source"] = "PTS"
                    else:
                        cine = DicomCine(path)
                        offsets, duration = cine.offsets_ms, cine.duration_ms
                        item["timing_source"] = cine.timing_source
                        item["transfer_syntax"] = cine.transfer_syntax.name
                    item.update(source_frames=len(offsets), last_frame_ms=float(offsets[-1]),
                                duration_ms=None if duration is None else float(duration))
                    if duration is None:
                        issue("duration_unavailable", "能核对首末帧和区间内抽帧，文件未给出可验证的末帧结束时间",
                              severity="warning", sheet="视频清单", row=clip.row, **locations)
                    if not rows:
                        issue("no_confirmed_intervals", "该文件没有可检查的已确认标注区间",
                              severity="warning", sheet="视频清单", row=clip.row, **locations)
                    decodes = {}
                    entries = []
                    for row in rows:
                        interval = {"sheet": "片段标注", "row": row.row, **locations,
                                    "video_row": clip.row, "segment_id": row.key,
                                    "start_ms": row.start_ms, "end_ms": row.end_ms,
                                    "sampled_frames": 0, "decoded": False}
                        checked.append(interval)
                        row_location = {"sheet": "片段标注", "row": row.row, **locations, "segment_id": row.key,
                                        "video_row": clip.row}
                        if duration is not None and row.end_ms > math.ceil(duration):
                            issue("interval_outside_media", f"结束时间超过原文件时长 {float(duration):g} ms",
                                  **row_location)
                            continue
                        # 保留边界两侧邻帧，由与训练共用的采样器判定时间窗。
                        start = max(0, bisect_left(offsets, row.start_ms) - 1)
                        end = min(len(offsets), bisect_left(offsets, row.end_ms) + 1)
                        timed = ((offsets[index], index) for index in range(start, end))
                        indices = [index for _, index in samples_in_intervals(timed, [row], [interval_bins(row, args)],
                                                                             exact_timing=cine is not None)]
                        if not indices:
                            issue("interval_has_no_frames", "该半开时间区间内没有可抽取的真实帧", **row_location)
                            continue
                        for index in indices:
                            decodes.setdefault(index, []).append(row_location)
                        interval["sampled_frames"] = len(indices)
                        interval["first_frame"] = {"number": indices[0] + 1, "offset_ms": float(offsets[indices[0]])}
                        interval["last_frame"] = {"number": indices[-1] + 1, "offset_ms": float(offsets[indices[-1]])}
                        entries.append(interval)
                    if cine is not None:
                        indices = sorted(decodes) if decodes else [0]
                        with closing(cine.decode(indices)) as frames:
                            for index in indices:
                                try:
                                    next(frames)
                                except MEDIA_INPUT_ERRORS as error:
                                    for location in decodes.get(index, ()):
                                        issue("cannot_decode_selected_frame", f"无法解码源帧 {index + 1}：{error}", **location)
                                    raise
                    item["decoded_frames"] = len(indices) if cine is not None else len(offsets)
                    for interval in entries:
                        interval["decoded"] = True
                    item["readable"] = True
            except MEDIA_INPUT_ERRORS as error:
                code = "unreadable_media" if item["exists"] else "missing_media"
                message = str(error) if item["exists"] else f"无法按相对路径找到或读取文件：{error}"
                issue(code, message, sheet="视频清单", row=clip.row, related_rows=[row.row for row in rows],
                      related_sheet="片段标注", **locations)
    return files, checked


def preflight(args):
    protected = {args.labels.resolve(), args.media.resolve()}
    if args.report.suffix.lower() != ".json" or args.report.resolve() in protected:
        raise ValueError("--report 需要指向独立的 .json 输出文件")
    issues = []

    def issue(code, message, severity="error", **location):
        issues.append({"severity": severity, "code": code, "message": message,
                       **{key: value for key, value in location.items() if value is not None}})

    try:
        labels, clips, segments, confirmed, pending, registered, dictionary_rows, checkable = read_annotations(args.labels, issue)
    except (OSError, BadZipFile, InvalidFileException, ParseError, KeyError, ValueError) as error:
        issue("unreadable_workbook", str(error), file=str(args.labels))
        labels, clips, segments, confirmed, pending, registered, dictionary_rows, checkable = {}, {}, {}, {}, [], [], {}, []

    if pending:
        issue("pending_intervals", "未确认的行不参与训练", severity="warning", sheet="片段标注", related_rows=pending)
    patient_rows = defaultdict(list)
    for clip_id, clip in registered:
        if clip.split:
            patient_rows[clip.patient].append({"row": clip.row, "file": clip.path, "clip_id": clip_id, "split": clip.split})
    split_conflict = False
    for rows in patient_rows.values():
        if len({row["split"] for row in rows}) > 1:
            split_conflict = True
            issue("patient_cross_split", "同一患儿被分到多个数据分组", sheet="视频清单", row=rows[0]["row"],
                  file=rows[0]["file"], related_rows=[row["row"] for row in rows], related_files=rows)
    if patient_rows:
        for clip_id, rows in segments.items():
            clip = clips[clip_id]
            if clip.patient not in patient_rows:
                split_conflict = True
                issue("missing_patient_split", "已启用手工分组，参与训练的患儿缺少 split",
                      sheet="视频清单", row=clip.row, file=clip.path, clip_id=clip_id,
                      related_rows=[row.row for row in rows], related_sheet="片段标注")

    files, checked = preflight_media(args, checkable, clips, confirmed, issue)
    resolved = {}
    assignments = {}

    def coverage_issue(code, message, missing=()):
        if missing:
            for task, label in missing:
                issue(code, f"训练组缺少 {task}/{label} 的已确认片段", sheet="标签字典",
                      row=dictionary_rows[task][label], task=task, label_code=label)
        else:
            issue(code, message, sheet="视频清单", row=4)

    if segments and not split_conflict and all(len(labels[task]) >= 2 for task in TASKS):
        try:
            assignments = patient_splits(clips, segments, labels, args.seed, args.val_fraction, coverage_issue)
            for split in ("train", "val", "test"):
                selected = [(clips[clip_id], rows) for clip_id, rows in segments.items()
                            if assignments[clips[clip_id].patient] == split]
                support = {task: dict.fromkeys(labels[task], 0) for task in TASKS}
                for _, rows in selected:
                    for row in rows:
                        for task, index in zip(TASKS, row.targets):
                            if index != IGNORE:
                                support[task][list(labels[task])[index]] += 1
                resolved[split] = {"patients": len({clip.patient for clip, _ in selected}),
                                   "files": len(selected), "labelled_intervals_per_class": support}
            for item in files:
                clip = clips.get(item["clip_id"])
                if clip is not None and clip.row == item["row"]:
                    item["effective_split"] = assignments.get(clip.patient)
        except ValueError as error:
            issue("split_label_coverage", str(error), sheet="视频清单", row=4, related_sheet="标签字典")
    errors = sum(item["severity"] == "error" for item in issues)
    report = {"status": "blocked" if errors else "passed", "workbook": str(args.labels), "media": str(args.media),
              "sample_fps": args.sample_fps, "max_frames_per_segment": args.max_frames_per_segment,
              "seed": args.seed, "val_fraction": args.val_fraction,
              "split_mode": "manual" if patient_rows else "patient_auto",
              "error_count": errors, "warning_count": len(issues) - errors,
              "files": files, "intervals": checked, "split_summary": resolved, "issues": issues}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2, default=str) + "\n", encoding="utf-8")
    print(f"预检{'未通过' if errors else '通过'}。{errors} 项错误，{len(issues) - errors} 项提示。报告 {args.report}")
    if errors:
        return None
    return TrainingInputs(labels, clips, segments, assignments)


def run_epoch(model, loader, sizes, optimizer=None, scaler=None):
    training = optimizer is not None
    model.train(training)
    model.features.eval()
    matrices = [torch.zeros((size, size), dtype=torch.int64, device="cuda") for size in sizes]
    loss_total = torch.zeros((), device="cuda")
    context = nullcontext() if training else torch.inference_mode()
    with context:
        for images, targets in loader:
            images = images.to("cuda", non_blocking=True)
            targets = targets.to("cuda", non_blocking=True)
            if training:
                optimizer.zero_grad(set_to_none=True)
            with torch.autocast("cuda", dtype=torch.float16):
                outputs = model(images)
                losses = [F.cross_entropy(logits, target, ignore_index=IGNORE, reduction="sum") /
                          target.ne(IGNORE).sum().clamp_min(1) for logits, target in zip(outputs, targets.T)]
                loss = sum(losses)
            if not torch.isfinite(loss).item():
                raise FloatingPointError("训练或验证损失出现非有限值")
            if training:
                scaler.scale(loss).backward()
                scaler.step(optimizer)
                scaler.update()
            loss_total += loss.detach()
            for matrix, logits, target, size in zip(matrices, outputs, targets.T, sizes):
                predicted = logits.detach().argmax(dim=1)
                valid = target.ne(IGNORE)
                pairs = size * target[valid] + predicted[valid]
                matrix += torch.bincount(pairs, minlength=size * size).reshape(size, size)
    metrics = {"loss": float(loss_total.item() / len(loader))}
    if not math.isfinite(metrics["loss"]):
        raise FloatingPointError("训练或验证损失出现非有限值")
    for task, matrix in zip(TASKS, matrices):
        matrix = matrix.cpu()
        support = matrix.sum(dim=1)
        active = support > 0
        f1 = (2 * matrix.diag()) / (support + matrix.sum(dim=0)).clamp_min(1)
        metrics[task] = {"macro_f1": float(f1[active].mean()), "support": support.tolist(), "confusion": matrix.tolist()}
    return metrics


def train(args, inputs):
    args.run_dir.mkdir(parents=True, exist_ok=False)
    torch.manual_seed(args.seed)
    torch.hub.set_dir(str(args.cache_dir / "torch-hub"))
    with tempfile.TemporaryDirectory(dir=args.run_dir) as directory:
        records = prepare_frames(args, inputs.clips, inputs.segments, inputs.assignments, Path(directory))
        normalization = WEIGHTS.transforms()
        loaders = {split: DataLoader(FrameDataset(records[split], normalization), batch_size=args.batch_size,
                                     shuffle=split == "train", pin_memory=True) for split in ("train", "val")}
        sizes = [len(inputs.labels[task]) for task in TASKS]
        model = EchoModel(*sizes).to("cuda")
        model.features.requires_grad_(False)
        optimizer = torch.optim.AdamW([
            {"params": list(model.view_head.parameters()) + list(model.quality_head.parameters()), "lr": args.head_lr},
            {"params": model.features.parameters(), "lr": args.backbone_lr},
        ])
        scaler = torch.amp.GradScaler("cuda")
        history = []
        best = -math.inf
        for epoch in range(args.epochs):
            if epoch == args.head_only_epochs:
                model.features[-3:].requires_grad_(True)
            training = run_epoch(model, loaders["train"], sizes, optimizer, scaler)
            validation = run_epoch(model, loaders["val"], sizes)
            score = sum(validation[task]["macro_f1"] for task in TASKS) / len(TASKS)
            row = {"epoch": epoch + 1, "train": training, "val": validation}
            history.append(row)
            print(json.dumps(row, ensure_ascii=False), flush=True)
            if score > best:
                best = score
                temporary = Path(directory) / "best.pt"
                torch.save({"model": model.state_dict(), "labels": inputs.labels, "epoch": epoch + 1,
                            "val_macro_f1": score, "preprocessing": "ffmpeg_area_rgb_letterbox_224_imagenet"}, temporary)
                temporary.replace(args.run_dir / "best.pt")
        (args.run_dir / "metrics.json").write_text(json.dumps(history, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def verify_logits(model, image, name):
    logits = model(image.to("cuda"))
    if any(tuple(head.shape) != (1, 2) or not torch.isfinite(head).all().item() for head in logits):
        raise RuntimeError(f"{name} 的双头前向输出异常")


def verify_dicom(cine, model, normalization, sample_fps):
    picks = list(dicom_samples_at_rate(cine.timed_indices(), sample_fps))
    indices = [index for _, index in picks]
    scaler = VideoReformatter()
    color = Counter()
    for index, frame in enumerate(cine.decode(indices)):
        resized = resize_and_pad(frame, scaler)
        image = normalize_image(resized, normalization).unsqueeze(0)
        if index == 0:
            verify_logits(model, image, cine.path.name)
        counts = color_counts(frame, resized, cine.color_regions)
        color.update(counts)
        color["sampled_frames_with_color_before"] += int(counts["rgb_color_pixels_before"] > 0)
        color["sampled_frames_with_color_after"] += int(counts["rgb_color_pixels_after"] > 0)
        color["sampled_frames_with_flow_color_before"] += int(counts["flow_color_pixels_before"] > 0)
        color["sampled_frames_with_flow_color_after"] += int(counts["flow_color_pixels_after"] > 0)
    return {"format": "DICOM", **cine.timing_report(), "decoded_frames": len(picks),
            "preprocessed_frames": len(picks), "cuda_forward_frames": 1,
            "sampled_source_frames": [{"frame_number": index + 1, "offset_ms": float(offset)}
                                      for offset, index in picks], "color_channel_spread_threshold_8bit": 8,
            "color": dict(color)}


def verify_video(source, model, normalization, sample_fps):
    scaler = VideoReformatter()
    decoded = 0
    sampled = 0
    last_slot = -1
    with av.open(str(source)) as container:
        stream = configure_video(container)
        for offset_ms, frame in video_frames(container, stream):
            decoded += 1
            slot = sampling_slot(offset_ms, sample_fps)
            if slot == last_slot:
                continue
            last_slot = slot
            image = preprocess(frame, normalization, scaler)
            sampled += 1
            if sampled == 1:
                verify_logits(model, image, source.name)
    if not sampled:
        raise ValueError(f"{source.name} 没有解码出有效帧")
    return {"format": "video", "source_frames": decoded, "decoded_frames": decoded,
            "preprocessed_frames": sampled, "cuda_forward_frames": 1}


def verify_media(args):
    if args.template:
        workbook_rows(args.template)
    torch.hub.set_dir(str(args.cache_dir / "torch-hub"))
    normalization = WEIGHTS.transforms()
    model = EchoModel(2, 2).eval().to("cuda")
    report = []
    skipped = []
    with open_archive(args.media) as archive, tempfile.TemporaryDirectory(dir=args.cache_dir) as scratch, torch.inference_mode():
        if archive is not None:
            paths = [entry.filename for entry in archive.infolist() if not entry.is_dir()]
        elif args.media.is_dir():
            paths = sorted(path.relative_to(args.media).as_posix() for path in args.media.rglob("*") if path.is_file())
        else:
            paths = [args.media.name]
        for name in paths:
            path = PurePosixPath(name)
            suffix = path.suffix.lower()
            if suffix not in MEDIA_SUFFIXES or "__MACOSX" in path.parts or path.name.upper() == "DICOMDIR":
                continue
            with local_media(args.media, archive, name, scratch) as source:
                if suffix in VIDEO_SUFFIXES:
                    checked = verify_video(source, model, normalization, args.sample_fps)
                else:
                    if not suffix and not is_dicom(source) and (archive is not None or args.media.is_dir()):
                        continue
                    header = dcmread(source, stop_before_pixels=True)
                    if archive is not None or args.media.is_dir():
                        sop = str(header.get("SOPClassUID", ""))
                        if sop not in US_CINE_SOP_CLASSES or int(header.get("NumberOfFrames", 0)) < 2:
                            skipped.append({"file": name, "sop_class_uid": sop})
                            continue
                    checked = verify_dicom(DicomCine(source, header), model, normalization, args.sample_fps)
            report.append({"video": name, **checked})
            print(json.dumps({"video": name, "format": checked["format"], "source_frames": checked["source_frames"],
                              "preprocessed_frames": checked["preprocessed_frames"]}, ensure_ascii=False), flush=True)
    if not report:
        raise ValueError("没有找到二维超声多帧 DICOM cine、MP4 或 MOV")
    result = {"sample_fps": args.sample_fps, "device": torch.cuda.get_device_name(), "videos": report,
              "source_frames": sum(row["source_frames"] for row in report), "skipped_dicom": skipped,
              "decoded_frames": sum(row["decoded_frames"] for row in report),
              "preprocessed_frames": sum(row["preprocessed_frames"] for row in report),
              "cuda_forward_frames": len(report), "template_schema_checked": bool(args.template)}
    args.report.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description="儿童心超切面识别和采集质量双头微调")
    commands = parser.add_subparsers(dest="command", required=True)
    media = commands.add_parser("verify-media", help="使用真实视频验证读取、预处理和 CUDA 前向")
    training = commands.add_parser("train", help="使用本次训练配置预检，通过后执行监督微调")
    checking = commands.add_parser("preflight", help="在 CPU 检查标注、文件、帧时间和患儿分组")
    for command in (media, training, checking):
        command.add_argument("--media", type=Path, required=True)
        command.add_argument("--cache-dir", type=Path, required=True)
        command.add_argument("--sample-fps", type=float, default=10)
    media.add_argument("--template", type=Path)
    media.add_argument("--report", type=Path, required=True)
    checking.add_argument("--report", type=Path, required=True)
    for command in (training, checking):
        command.add_argument("--labels", type=Path, required=True)
        command.add_argument("--max-frames-per-segment", type=int, default=8)
        command.add_argument("--val-fraction", type=float, default=0.2)
        command.add_argument("--seed", type=int, default=42)
    training.add_argument("--run-dir", type=Path, required=True)
    training.add_argument("--report", type=Path, help="预检报告路径，默认在训练目录旁生成 <目录名>.preflight.json")
    training.add_argument("--preflight-only", action="store_true", help="使用本次训练配置写出预检报告后退出")
    training.add_argument("--epochs", type=int, default=12)
    training.add_argument("--head-only-epochs", type=int, default=2)
    training.add_argument("--batch-size", type=int, default=8)
    training.add_argument("--head-lr", type=float, default=3e-4)
    training.add_argument("--backbone-lr", type=float, default=3e-5)
    args = parser.parse_args()
    if not math.isfinite(args.sample_fps) or args.sample_fps <= 0:
        parser.error("--sample-fps 需要为有限正数")
    if args.command in {"train", "preflight"} and (args.max_frames_per_segment <= 0 or not 0 < args.val_fraction < 1):
        parser.error("每片段帧数和验证组比例需有效")
    if args.command == "preflight":
        args.cache_dir.mkdir(parents=True, exist_ok=True)
        return 1 if preflight(args) is None else 0
    if args.command == "train":
        if min(args.epochs, args.batch_size) <= 0 or args.head_only_epochs < 0:
            parser.error("轮数和批大小需有效")
        if any(not math.isfinite(lr) or lr <= 0 for lr in (args.head_lr, args.backbone_lr)):
            parser.error("学习率需有效")
        run_dir = args.run_dir.resolve()
        if args.cache_dir.resolve().is_relative_to(run_dir):
            parser.error("缓存目录应放在训练目录之外")
        if args.report is None:
            args.report = run_dir.parent / f"{run_dir.name}.preflight.json"
        if args.report.resolve().is_relative_to(run_dir):
            parser.error("预检报告应放在训练目录之外")
        args.cache_dir.mkdir(parents=True, exist_ok=True)
        inputs = preflight(args)
        if inputs is None:
            return 1
        if args.preflight_only:
            return 0
    if not torch.cuda.is_available():
        raise RuntimeError("当前 PyTorch 无法使用 CUDA")
    torch.set_num_threads(1)
    if args.command == "train":
        train(args, inputs)
    else:
        args.cache_dir.mkdir(parents=True, exist_ok=True)
        verify_media(args)


if __name__ == "__main__":
    raise SystemExit(main())
