import math


def validate_labels(info, key):
    if not isinstance(info, dict):
        raise ValueError(f"嵌入缺少标签：{key}")
    subject = info.get("subject")
    if not isinstance(subject, str) or not subject.strip() or subject != subject.strip():
        raise ValueError(f"嵌入需要有效的 subject：{key}")
    ef = info.get("ef")
    if type(ef) not in (int, float) or not 0 < ef <= 100 or not math.isfinite(ef):
        raise ValueError(f"嵌入需要 0 到 100 之间的 EF 百分比（0 除外）：{key}")
    if info.get("split", "train") not in ("train", "val", "test"):
        raise ValueError(f"嵌入的 split 需要为 train、val 或 test：{key}")
