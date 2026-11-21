import argparse
from datetime import date
from hashlib import file_digest
import json
import math
import os
from pathlib import Path
import re
from tempfile import NamedTemporaryFile

from pydantic import ValidationError

from backend.multiview_coverage import view_coverage
from multiview_study import GovernanceError, evaluate, governance


PUBLIC_SOURCE = "https://pocusjournal.com/article/2021-06-01p10-12/"
PUBLIC_CLIPS = (
    ("S3", "A4C", "Damji-et-al-Video-S3-A4C-TV-Endo-1.mp4",
     "543c6a330eda334e498d8bb2ee7880a46a899c660b5b669d7613ee746d4a9d6c"),
    ("S1", "PLAX", "Damji-et-al-Video-S1-PSLX-1.mp4",
     "dddbf2def6a7923c1348fc2ccdb398e08b3a2e414d6ea86e1515349ababe9bff"),
)


def approved_study(identifier):
    if not re.fullmatch(r"[a-zA-Z0-9][a-zA-Z0-9_-]{0,63}", identifier):
        raise GovernanceError("invalid_study_id")
    root = Path(os.environ.get("ECHO_MULTIVIEW_REVIEW_ROOT", ""))
    if not root.is_absolute():
        raise GovernanceError("protocol_approval_missing, data_authorization_missing: 未配置受管审定目录")
    root = root.resolve()
    study = (root / identifier).resolve()
    if study.parent != root:
        raise GovernanceError("study_outside_governance_root")
    approval, authorization = governance(study, date.today())
    return study, approval, authorization


def public_frames(directory):
    import av

    observations = []
    for media_id, view, filename, expected_sha256 in PUBLIC_CLIPS:
        path = directory / filename
        with path.open("rb") as video:
            if file_digest(video, "sha256").hexdigest() != expected_sha256:
                raise ValueError(f"public_sample_digest_mismatch: {media_id}")
            video.seek(0)
            with av.open(video) as container:
                stream = container.streams.video[0]
                times = [frame.time for frame in container.decode(stream)]
                if not times or any(time is None or not math.isfinite(time) for time in times):
                    raise ValueError(f"public_sample_frame_unavailable: {media_id}")
        offsets = [(time - times[0]) * 1000 for time in times]
        if offsets != sorted(offsets):
            raise ValueError(f"public_sample_invalid_timeline: {media_id}")
        frame = {"media_id": media_id, "view": view, "frame_index": 0, "timestamp_ms": offsets[0]}
        observations.append((frame, offsets))
    return observations


def verify_public(directory):
    frames = [frame for frame, _ in public_frames(directory)]
    required = [view for _, view, _, _ in PUBLIC_CLIPS]
    scenarios = {}
    cases = (
        ("both_views", frames, "succeeded"),
        ("plax_withheld", frames[:1], "insufficient_evidence"),
    )
    for name, observed, expected in cases:
        coverage = view_coverage(required, (frame["view"] for frame in observed))
        state = "insufficient_evidence" if "missing" in coverage.values() else "succeeded"
        if state != expected:
            raise ValueError(f"unexpected_public_coverage: {name}")
        scenarios[name] = {"state": state, "coverage": coverage, "confirmed_frames": observed}
        if state == "insufficient_evidence":
            scenarios[name]["insufficient_evidence"] = {"reason": "coverage"}
    return {
        "scope": "public_workflow_verification",
        "source": PUBLIC_SOURCE,
        "source_views": required,
        "scenarios": scenarios,
        "verified_cases": len(scenarios),
    }


def save_report(path, report):
    data = json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False) + "\n"
    target = NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent, delete=False)
    staged = Path(target.name)
    try:
        with target:
            target.write(data)
        os.link(staged, path)
    finally:
        staged.unlink()


def main():
    parser = argparse.ArgumentParser(description="独立多切面评价和公开样例流程验收")
    commands = parser.add_subparsers(dest="command", required=True)
    clinical = commands.add_parser("evaluate", help="核对机构审定与数据授权后，评价冻结结果")
    clinical.add_argument("--study", required=True)
    public = commands.add_parser("verify-public", help="只核对已登记公开教学影像的覆盖及拒判流程")
    public.add_argument("--media-dir", type=Path, required=True)
    for command in (clinical, public):
        command.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == "evaluate":
            study, approval, authorization = approved_study(args.study)
            if args.report.resolve().is_relative_to(study.parent):
                raise ValueError("report_inside_governance_root")
        if args.report.suffix.lower() != ".json" or args.report.exists():
            raise ValueError("report_must_be_new_json")
        report = (evaluate(study, approval, authorization) if args.command == "evaluate"
                  else verify_public(args.media_dir))
        save_report(args.report, report)
    except ValidationError as error:
        print("评价已阻断 invalid_frozen_input: " + ", ".join(sorted({item["type"] for item in error.errors()})))
        return 1
    except (OSError, ValueError, IndexError) as error:
        print(f"评价已阻断 {error}")
        return 1
    print(f"已保存 {report['scope']} 报告 {args.report}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
