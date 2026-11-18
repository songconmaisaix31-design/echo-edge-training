import argparse
import io
import json
import re
from pathlib import Path
from zipfile import ZipFile

import av


PATIENT_FIELDS = {
    "birth": "出生日期",
    "business_number": "脱敏业务编号",
    "notes": "备注",
    "history": "病史",
    "treatment_history": "治疗史",
}
CLINICAL_FIELDS = {
    "main_problem": "主要问题",
    "congenital_diagnosis": "先天性疾病记录",
    "other_diagnosis": "其他疾病记录",
    "birth_weight_g": "出生体重（g）",
}
EXAM_FIELDS = {"exam_date": "检查日期", "height_cm": "身高（cm）", "weight_kg": "体重（kg）",
               "clinical_notes": "检查备注"}
SEX = {"female": "女", "male": "男", "unknown": "未记录"}


def require(condition, detail):
    if not condition:
        raise ValueError(detail)


def fields(row, labels):
    return [{"label": label, "value": str(row[name])} for name, label in labels.items()
            if type(row.get(name)) in (int, float, str) and str(row[name]).strip()]


def export_video(source, output, count):
    with av.open(io.BytesIO(source)) as input_video, av.open(str(output), "w", options={"movflags": "+faststart"}) as public:
        stream = input_video.streams.video[0]
        rate = stream.average_rate or stream.base_rate
        require(rate is not None and rate > 0, "视频缺少可用帧率")
        target = public.add_stream("libx264", rate=rate, options={"crf": "18", "preset": "fast"})
        target.width = stream.width + stream.width % 2
        target.height = stream.height + stream.height % 2
        target.pix_fmt = "yuv420p"
        decoded = 0
        for frame in input_video.decode(stream):
            image = frame.reformat(width=target.width, height=target.height, format=target.pix_fmt)
            image.pts = None
            public.mux(target.encode(image))
            decoded += 1
        require(decoded == count, "公开视频的解码帧数与样例目录不一致")
        public.mux(target.encode())


def export_frames(video, directory, number, count):
    directory.mkdir(parents=True)
    with av.open(str(video)) as media:
        stream = media.streams.video[0]
        frames = []
        for index, frame in enumerate(media.decode(stream)):
            require(frame.time is not None, "公开视频缺少帧时间")
            frame.to_image().save(directory / f"{index}.jpg", quality=82)
            frames.append({"index": index, "offsetMs": frame.time * 1000})
        require(len(frames) == count, "公开视频帧索引不完整")
        metadata = {"id": str(number), "width": stream.width, "height": stream.height,
                    "codec": stream.codec_context.name, "frameCount": count, "frames": frames}
    (directory / "metadata.json").write_text(json.dumps(metadata, allow_nan=False), encoding="utf-8")


def catalog_record(source, output):
    records = json.loads(source.read("records.json"))
    counts = {"patients": 4, "examinations": 4, "media": 5}
    require(all(len(records[key]) == count for key, count in counts.items()), "只可发布获准的四份档案、四条检查和五段视频")
    require(all(len({row["id"] for row in records[key]}) == count for key, count in counts.items()), "样例编号冲突")
    patient_ids = {row["id"] for row in records["patients"]}
    exam_ids = {row["id"] for row in records["examinations"]}
    require(all(row["patient_id"] in patient_ids for row in records["examinations"])
            and all(row["case_id"] in exam_ids for row in records["media"]), "公开样例的关联范围不完整")

    videos = {}
    for number, row in enumerate(records["media"], 1):
        identifier = row["id"]
        require(re.fullmatch(r"[0-9a-f]{32}", identifier), "无效影像编号")
        folder = f"media/{identifier}"
        approved = json.loads(source.read(f"{folder}/metadata.json"))
        count = approved["frameCount"]
        require(type(count) is int and count > 0, "无效帧数")
        public_video = output / f"video-{number}.mp4"
        export_video(source.read(f"{folder}/source.mp4"), public_video, count)
        export_frames(public_video, output / "frames" / str(number), number, count)
        videos.setdefault(row["case_id"], []).append({"id": number, "frames": count})

    latest = {row["exam_id"]: json.loads(row["data"])
              for row in sorted(records["examination_parameters"], key=lambda item: item["id"])}
    exams = {}
    for number, row in enumerate(records["examinations"], 1):
        attributes = fields(row, {"reason": "检查指征"}) + fields(latest.get(row["id"], {}), EXAM_FIELDS)
        exams.setdefault(row["patient_id"], []).append({"id": number, "kind": row["kind"], "fields": attributes,
                                                        "videos": videos.get(row["id"], [])})

    archives = []
    for number, row in enumerate(records["patients"], 1):
        attributes = [{"label": "性别", "value": SEX[row["sex"]]}]
        attributes += fields(row, PATIENT_FIELDS) + fields(json.loads(row["clinical_data"]), CLINICAL_FIELDS)
        archives.append({"id": number, "name": row["name"], "fields": attributes, "exams": exams.get(row["id"], [])})
    return {"archives": archives}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("archive", type=Path)
    parser.add_argument("dist", type=Path)
    args = parser.parse_args()
    require((args.dist / "index.html").is_file(), "需要已构建的前端 dist 目录")
    output = args.dist / "samples"
    output.mkdir()
    with ZipFile(args.archive) as source:
        data = catalog_record(source, output)
    (output / "catalog.json").write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")


if __name__ == "__main__":
    main()
