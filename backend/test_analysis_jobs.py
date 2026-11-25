from types import SimpleNamespace

import main
import model_inference
import platform_db as db
from clinical import Examination, Patient
from fastapi import HTTPException
from fastapi.testclient import TestClient
from sqlalchemy import create_engine
from sqlalchemy.orm import sessionmaker


def test_job_failure_and_history_isolation(tmp_path, monkeypatch):
    engine = create_engine("sqlite:///" + str(tmp_path / "jobs.db"))
    db.Base.metadata.create_all(engine)
    sessions = sessionmaker(engine, expire_on_commit=False)
    monkeypatch.setattr(db, "Session", sessions)
    monkeypatch.setattr(main, "ROOT", tmp_path)
    ids = ["a" * 32, "b" * 32]
    with sessions() as session:
        session.add(Patient(id=1, name="fixture", birth="2020-01-01", sex="unknown",
                            owner=1, notes="", history="", treatment_history=""))
        for key, identifier in enumerate(ids, 1):
            session.add(db.Case(id=key, title="fixture", notes="", status="draft", owner=key))
            session.add(Examination(id=key, patient_id=1, snapshot="{}", kind="US",
                                    reason="", created="2024-01-01"))
            session.add(db.Media(id=identifier, case_id=key))
        session.commit()

    checkpoint = tmp_path / "model.pt"
    checkpoint.write_bytes(b"test artifact")
    folder = tmp_path / ids[0]
    folder.mkdir()
    (folder / "source.mp4").write_bytes(b"test input")
    monkeypatch.setattr(model_inference, "configuration",
                        lambda: ("test", (None, None, None, checkpoint)))

    def timeout(_):
        raise HTTPException(504, "test timeout")

    monkeypatch.setattr(model_inference, "predict", timeout)
    client = TestClient(main.app)
    user = SimpleNamespace(id=1, role="operator")
    monkeypatch.setitem(main.app.dependency_overrides, db.current, lambda: user)
    endpoint = "/api/v1/media/" + ids[0]
    response = client.post(endpoint + "/analysis-jobs")
    assert response.status_code == 201
    job = response.json()
    assert job["state"] == "failed" and job["error"] == "test timeout"
    assert job["completed"] and job["output"] is None
    assert client.get("/api/v1/examinations/1/analysis-jobs").json() == [job]
    assert client.get("/api/v1/examinations/2/analysis-jobs").status_code == 404
    assert client.get("/api/v1/media/" + ids[1] + "/analysis-jobs").status_code == 404
    assert client.post(endpoint + "/predict").status_code == 504

    user.id = 2
    assert client.get("/api/v1/analysis-jobs/" + str(job["id"])).status_code == 404
    assert client.get("/api/v1/examinations/2/analysis-jobs").json() == []
    assert client.post("/api/v1/media/" + ids[1] + "/analysis-jobs").status_code == 422
from datetime import timedelta

import pytest
from analysis_jobs import RUNNER_MARKER, AnalysisJob, recover_jobs, runner_lock


def test_startup_recovery_preserves_completed_jobs(tmp_path, monkeypatch):
    engine = create_engine("sqlite:///" + str(tmp_path / "recovery.db"))
    db.Base.metadata.create_all(engine)
    sessions = sessionmaker(engine, expire_on_commit=False)
    monkeypatch.setattr(db, "engine", engine)
    monkeypatch.setattr(db, "Session", sessions)
    now = db.now()
    with sessions.begin() as session:
        for state, error, created in [
            ("succeeded", None, now),
            ("running", RUNNER_MARKER, now),
            ("running", None, now - timedelta(minutes=6)),
            ("running", None, now),
        ]:
            session.add(AnalysisJob(
                exam_id=1, media_id="a"*32, model_type="stanford_view",
                model_version="test", input_sha256="a"*64, state=state,
                error=error, output='{"view":"A4C"}' if state=="succeeded" else None,
                actor=1, created=created,
            ))
    with TestClient(main.app):
        with pytest.raises(RuntimeError, match="runner lock"):
            with runner_lock():
                pass
    with TestClient(main.app):
        recover_jobs()
    with sessions() as session:
        jobs = [session.get(AnalysisJob, i) for i in range(1, 5)]
        assert [job.state for job in jobs] == ["succeeded", "interrupted", "interrupted", "running"]
        assert jobs[0].output == '{"view":"A4C"}'
        assert jobs[1].completed and jobs[1].error and jobs[1].output is None
