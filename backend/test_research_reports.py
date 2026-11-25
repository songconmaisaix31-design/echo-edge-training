import main
import platform_db as db
from analysis_jobs import AnalysisJob
from fastapi.testclient import TestClient
from sqlalchemy import create_engine
from sqlalchemy.orm import sessionmaker


def test_consultation_review_and_immutable_report(tmp_path, monkeypatch):
    engine = create_engine("sqlite:///" + str(tmp_path / "workflow.db"))
    db.Base.metadata.create_all(engine)
    sessions = sessionmaker(engine, expire_on_commit=False)
    monkeypatch.setattr(db, "Session", sessions)
    with sessions.begin() as session:
        for role in ("operator", "reviewer"):
            salt = "ab" * 16
            session.add(db.User(name=role, role=role, password=salt+":"+db.digest("test-password", salt)))
    client = TestClient(main.app)

    def login(role):
        assert client.post("/api/v1/login", json={"name":role,"password":"test-password"}).status_code == 200

    login("operator")
    patient = client.post("/api/v1/patients", json={"name":"Fixture","birth":"2020-01-01","sex":"unknown"}).json()
    key, other = [client.post("/api/v1/examinations", json={"patient_id":patient["id"],"kind":"US"}).json()["id"]
                  for _ in range(2)]
    case, exam = f"/api/v1/cases/{key}", f"/api/v1/examinations/{key}"
    with sessions.begin() as session:
        for owner in (key, other):
            media = str(owner).zfill(32)
            session.add(db.Media(id=media, case_id=owner))
            session.add(AnalysisJob(exam_id=owner,media_id=media,model_type="stanford_view",
                                    model_version="test",input_sha256="a"*64,state="succeeded",
                                    output='{"view":"A4C"}',created=db.now(),actor=1))
    draft = {"kind":"report","text":"Legacy draft"}
    assert client.post(case+"/entries",json=draft).status_code == 201
    assert client.post(case+"/entries",json={"kind":"consultation","state":"submitted","text":"Question"}).status_code == 201
    issue = {"review_id":1,"text":"Research report","analysis_ids":[1]}
    assert client.post(exam+"/report-versions",json=issue).status_code == 403
    login("reviewer")
    assert client.post(case+"/entries",json={"kind":"consultation","state":"feedback","text":"Advice"}).status_code == 201
    assert client.get(case).json()["status"] == "draft"
    assert client.post(case+"/reviews",json={"text":"Premature"}).status_code == 409
    login("operator")
    assert client.post(case+"/submit").json()["status"] == "submitted"
    assert client.post(case+"/reviews",json={"text":"Unauthorized"}).status_code == 403
    login("reviewer")
    assert client.post(case+"/reviews",json={"text":"First conclusion"}).json()["status"] == "reviewed"
    assert client.post(exam+"/report-versions",json={**issue,"analysis_ids":[2]}).status_code == 422
    first = client.post(exam+"/report-versions",json=issue)
    assert first.status_code == 201, first.text
    original = first.json()
    assert original["review"]["text"] == "First conclusion"
    assert original["analysis"][0]["id"] == 1
    assert client.post(case+"/reviews",json={"text":"Second conclusion"}).status_code == 200
    second = client.post(exam+"/report-versions",json={**issue,"review_id":2}).json()
    assert second["version"] != original["version"]
    assert client.get(exam+"/report-versions").json() == [second, original]
    export = exam+f"/report-versions/{original['id']}/export"
    assert "First conclusion" in client.get(export).text
    assert "Second conclusion" not in client.get(export).text
    assert client.get(export.replace(f"/{key}/",f"/{other}/",1)).status_code == 404
    history = client.get(f"/api/v1/patients/{patient['id']}/reports").json()
    assert [r["state"] for r in history] == ["issued","issued","saved"]
    assert "Legacy draft" in client.get(case+"/report-export").text
