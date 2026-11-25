import json
import os
import sqlite3
from contextlib import closing
from datetime import datetime
from pathlib import Path

import platform_db as db
import pytest
from analysis_contract import AnalysisJobResponse
from analysis_jobs import AnalysisJob, execute_job, serialize
from medical_review import STEPFUN_MODEL
from model_protocol import ModelInput
from pydantic import TypeAdapter
from sqlalchemy import create_engine


def test_approved_review_indeterminate():
    database = os.environ.get("ECHO_VERIFY_REVIEW_DB")
    samples = os.environ.get("ECHO_VERIFY_REVIEW_SAMPLES")
    if database is None and samples is None:
        pytest.skip("需要已获准公开样例的复核历史数据库和 SHA-256 清单")
    assert database and samples, "须同时指定历史数据库和 SHA-256 清单"
    source = Path(database).resolve(strict=True)
    approved = json.loads(Path(samples).read_text(encoding="utf-8"))
    with closing(sqlite3.connect(source.as_uri() + "?mode=ro&immutable=1", uri=True)) as archive:
        archive.row_factory = sqlite3.Row
        for identifier, fingerprint in approved.items():
            row = archive.execute(
                "SELECT * FROM analysis_jobs WHERE media_id = ? AND input_sha256 = ? "
                "AND model_type = 'llm_findings' AND state IN ('succeeded', 'insufficient_evidence') "
                "AND json_extract(output, '$.model') = ? AND json_extract(output, '$.assessment') = 'indeterminate' "
                "ORDER BY id DESC LIMIT 1",
                (identifier, fingerprint, STEPFUN_MODEL),
            ).fetchone()
            if row is not None:
                break
        else:
            pytest.fail("未找到使用阶跃星辰模型并返回 indeterminate 的获准公开样例记录")

    output = json.loads(row["output"])
    contract = TypeAdapter(AnalysisJobResponse)
    archived = dict(row)
    for field in ("created", "completed"):
        archived[field] = datetime.fromisoformat(archived[field])
    legacy = serialize(AnalysisJob(**archived))
    assert contract.validate_python(legacy).state == "insufficient_evidence"
    assert legacy["insufficient_evidence"]["reason"] == "coverage"
    assert legacy["insufficient_evidence"]["next_step"]

    engine = create_engine("sqlite://")
    db.Base.metadata.create_all(engine)
    original_bind = db.Session.kw["bind"]
    db.Session.configure(bind=engine)
    try:
        # 复用历史任务已保存的真实模型响应，重新执行结果判定与写入。
        model_input = ModelInput(row["exam_id"], row["media_id"], source, row["input_sha256"])
        result, failure = execute_job(model_input, db.User(id=row["actor"]), row["model_type"],
                                      row["model_version"], lambda _job_id: output)
        assert failure is None and result["error"] is None
        assert contract.validate_python(result).state == "insufficient_evidence"
        assert result["insufficient_evidence"] == legacy["insufficient_evidence"]
        assert result["output"] == legacy["output"]
        assert result["output"]["assessment"] == "indeterminate"
        assert result["output"]["sampled_frames"] == output["sampled_frames"]
        assert not {"summary", "observations", "limitations", "recommendations"}.intersection(result["output"])
        with db.Session() as session:
            stored = session.get(AnalysisJob, result["id"])
            assert stored.state == "insufficient_evidence"
            assert json.loads(stored.output) == output
            assert serialize(stored) == result
    finally:
        db.Session.configure(bind=original_bind)
        engine.dispose()
