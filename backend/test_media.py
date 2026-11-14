from sqlalchemy import create_engine
from sqlalchemy.orm import sessionmaker
import platform_db as db

import av
from fastapi.testclient import TestClient
from PIL import Image

import main

def test_media_round_trip(tmp_path, monkeypatch):
    monkeypatch.setattr(main, "ROOT", tmp_path / "store")
    source = tmp_path / "fixture.mp4"
    with av.open(str(source), "w") as container:
        stream = container.add_stream("mpeg4", rate=25)
        stream.width, stream.height = 64, 48
        stream.pix_fmt = "yuv420p"
        for shade in (0, 90, 180):
            frame = av.VideoFrame.from_image(Image.new("RGB", (64, 48), (shade,) * 3))
            for packet in stream.encode(frame):
                container.mux(packet)
        for packet in stream.encode():
            container.mux(packet)
    engine = create_engine("sqlite:///" + str(tmp_path / "test.db"))
    db.Base.metadata.create_all(engine)
    sessions = sessionmaker(engine, expire_on_commit=False)
    monkeypatch.setattr(db, "Session", sessions)
    monkeypatch.setattr(main, "Session", sessions)
    password = "isolated-test-password"
    salt = "12" * 16
    with sessions() as session:
        for role in ("operator", "reviewer", "admin"):
            session.add(db.User(name=role,role=role,password=salt+":"+db.digest(password,salt)))
        session.commit()
    client = TestClient(main.app)
    assert client.get("/api/v1/cases").status_code == 401
    assert client.post("/api/v1/login",json={"name":"operator","password":password}).status_code == 200
    case_id = client.post("/api/v1/cases",json={"title":"Test case"}).json()["id"]
    assert client.put(f"/api/v1/cases/{case_id}",json={"title":"Edited case"}).status_code == 200
    assert len(client.get("/api/v1/cases?q=Edited&status=draft").json()) == 1
    result = client.post("/api/v1/media", data={"case_id":case_id}, files={"media": ("fixture.mp4", source.read_bytes())})
    assert result.status_code == 201, result.text
    metadata = result.json()
    assert metadata["frameCount"] == 3
    assert (metadata["width"], metadata["height"]) == (64, 48)
    assert [frame["offsetMs"] for frame in metadata["frames"]] == [0, 40, 80]
    url = "/api/v1/media/" + metadata["id"]
    assert client.get(url).json() == metadata
    assert client.get(url + "/frames/1").content.startswith(b"\xff\xd8")
    assert client.get(url + "/frames/3").status_code == 404
    assert client.get("/api/v1/capabilities").json()["model"] == "unavailable"
    assert client.post("/api/v1/media", data={"case_id":case_id}, files={"media": ("file.dcm", b"x")}).status_code == 415
    assert client.post("/api/v1/media", data={"case_id":case_id}, files={"media": ("file.mp4", b"x")}).status_code == 422
    monkeypatch.setattr(main, "LIMIT", 2)
    assert client.post("/api/v1/media", data={"case_id":case_id}, files={"media": ("file.mp4", b"123")}).status_code == 413

    assert client.post(f"/api/v1/cases/{case_id}/submit").status_code == 200
    assert client.post(f"/api/v1/cases/{case_id}/reviews",json={"text":"Reviewed"}).status_code == 403
    assert client.post("/api/v1/logout").status_code == 200
    assert client.get(url+"/frames/0").status_code == 401
    assert client.post("/api/v1/login",json={"name":"reviewer","password":password}).status_code == 200
    assert client.post(f"/api/v1/cases/{case_id}/reviews",json={"text":"Manual verification"}).status_code == 200
    report = client.get(f"/api/v1/cases/{case_id}/report")
    assert report.status_code == 200 and "Manual verification" in report.text
    assert report.headers["content-disposition"].startswith("attachment")
    assert client.get("/api/v1/audit").status_code == 403
    assert client.post("/api/v1/login",json={"name":"admin","password":password}).status_code == 200
    assert any(row["action"] == "report.export" for row in client.get("/api/v1/audit").json())
