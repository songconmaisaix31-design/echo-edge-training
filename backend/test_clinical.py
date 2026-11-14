import pytest
from sqlalchemy import create_engine, select
from sqlalchemy.orm import sessionmaker
from fastapi.testclient import TestClient
import platform_db as p
import main
from clinical import Patient, Examination, Entry

def test_clinical_flow(tmp_path, monkeypatch):
    engine = create_engine("sqlite:///"+str(tmp_path/"clinical.db"))
    p.Base.metadata.create_all(engine)
    factory = sessionmaker(engine, expire_on_commit=False)
    monkeypatch.setattr(p, "Session", factory)
    monkeypatch.setattr(main, "Session", factory)
    monkeypatch.setattr(main, "ROOT", tmp_path/"media")
    with factory() as db:
        for name,role in [("doctor","operator"),("other","operator"),("expert","reviewer"),("admin","admin")]:
            salt="12"*16
            db.add(p.User(name=name,role=role,password=salt+":"+p.digest("test-password-123",salt)))
        db.commit()
    client=TestClient(main.app)
    def login(name):
        assert client.post("/api/v1/login",json={"name":name,"password":"test-password-123"}).status_code==200
    login("doctor")
    body={"name":"Synthetic subject","birth":"2020-01-01","sex":"unknown","notes":"test"}
    patient=client.post("/api/v1/patients",json=body).json()
    ids=[]
    for reason in ("first","second"):
        response=client.post("/api/v1/examinations",json={"patient_id":patient["id"],"kind":"US","reason":reason})
        assert response.status_code==201,response.text
        ids.append(response.json()["id"])
    body["name"]="Edited subject"
    assert client.put("/api/v1/patients/"+str(patient["id"]),json=body).status_code==200
    exams=client.get("/api/v1/examinations").json()
    assert len(exams)==2 and exams[0]["snapshot"]["name"]=="Synthetic subject"
    path=f"/api/v1/cases/{ids[0]}/entries"
    for kind in ("review","report","organization"):
        assert client.post(path,json={"kind":kind,"text":"saved "+kind}).status_code==201
    assert client.post(path,json={"kind":"consultation","text":"request","state":"submitted"}).status_code==201
    assert client.post(path,json={"kind":"consultation","text":"fake expert","state":"feedback"}).status_code==403
    login("other")
    assert client.get(path).status_code==404
    assert client.get("/api/v1/patients").json()==[]
    login("expert")
    assert client.post(path,json={"kind":"consultation","text":"expert opinion","state":"feedback"}).status_code==201
    login("doctor")
    assert client.post(path,json={"kind":"consultation","text":"closed","state":"closed"}).status_code==201
    assert len(client.get(path).json())==6
    assert "expert opinion" in client.get(f"/api/v1/cases/{ids[0]}/report-export").text
    assert client.get("/api/v1/users").status_code==403
    login("admin")
    created=client.post("/api/v1/users",json={"name":"new","password":"test-password-123","role":"operator"})
    assert created.status_code==201
    assert client.put("/api/v1/users/"+str(created.json()["id"])+"/role",json={"role":"reviewer"}).status_code==200
    assert len(client.get("/api/v1/audit").json())>5
    with factory() as db:
        assert db.scalar(select(Patient)).name=="Edited subject"
        assert len(db.scalars(select(Examination)).all())==2

    login("doctor")
    parameters={"exam_date":"2024-01-01","height_cm":100,"weight_kg":36,"heart_rate":90,"systolic":100,"diastolic":60,"spo2":98}
    parameter_path=f"/api/v1/examinations/{ids[0]}/parameters"
    response=client.post(parameter_path,json=parameters)
    assert response.status_code==200,response.text
    assert response.json()["bsa_m2"]==1
    assert response.json()["age_days"]==1461
    assert client.get(f"/api/v1/examinations/{ids[1]}/parameters").json()==[]
    assert client.post(parameter_path,json={**parameters,"height_cm":None}).json()["bsa_m2"] is None
    assert len(client.get(parameter_path).json())==2
    for invalid in ({"weight_kg":-1},{"spo2":101},{"systolic":None},{"diastolic":110},{"exam_date":"2019-01-01"}):
        assert client.post(parameter_path,json={**parameters,**invalid}).status_code==422
    measure_path=f"/api/v1/examinations/{ids[0]}/measurements"
    measurement={"item":"LV dimension","value":12.5,"unit":"mm","view":"A4C","phase":"ED","source":"manual","review":"pending"}
    assert client.post(measure_path,json=measurement).status_code==201
    assert client.get(measure_path).json()[0]["value"]==12.5
    assert client.post(measure_path,json={**measurement,"source":"ai"}).status_code==409
    assert client.post(measure_path,json={**measurement,"frame_index":0}).status_code==422
    assert client.get(f"/api/v1/examinations/{ids[0]}/z-score").json()["value"] is None
    original=client.get(measure_path).json()[0]
    response=client.post(measure_path,json={**measurement,"value":14,"replaces_id":original["id"],"review":"confirmed"})
    assert response.status_code==201
    records=client.get(measure_path).json()
    assert records[0]["replaces_id"]==original["id"]
    assert records[0]["value"]==14 and records[1]["value"]==12.5
    assert client.post(measure_path,json={**measurement,"replaces_id":99999}).status_code==422
