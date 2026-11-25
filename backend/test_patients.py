import main
import platform_db as p
from fastapi.testclient import TestClient
from sqlalchemy import create_engine
from sqlalchemy.orm import sessionmaker


def test_patient_archive(tmp_path, monkeypatch):
    engine=create_engine('sqlite:///'+str(tmp_path/'patients.db'))
    p.Base.metadata.create_all(engine)
    factory=sessionmaker(engine,expire_on_commit=False)
    monkeypatch.setattr(p,'Session',factory)
    monkeypatch.setattr(main,'Session',factory)
    with factory() as db:
        for name,role in [('owner','operator'),('other','operator'),('reviewer','reviewer')]:
            salt='12'*16
            db.add(p.User(name=name,role=role,password=salt+':'+p.digest('test-password-123',salt)))
        db.commit()
    client=TestClient(main.app)
    def login(name):
        assert client.post('/api/v1/login',json={'name':name,'password':'test-password-123'}).status_code==200
    login('owner')
    body={'name':'Synthetic test only','birth':'2020-01-01','sex':'unknown','business_number':'TEST-001','history':'original history','clinical':{'events':[{'kind':'surgery','description':'test event','date':'2021-02-03'}]}}
    result=client.post('/api/v1/patients',json=body)
    assert result.status_code==201,result.text
    patient = result.json()
    key = patient['id']
    path = f'/api/v1/patients/{key}'
    assert patient['business_number']=='TEST-001' and patient['created']
    assert client.post('/api/v1/patients',json=body).status_code==409
    assert len(client.get('/api/v1/patients?q=TEST-001').json())==1
    assert client.get(path).json()['can_edit']
    ids=[]
    for weight in [9,12]:
        r=client.post('/api/v1/examinations',json={'patient_id':key,'kind':'Test','parameters':{'exam_date':'2024-01-01','height_cm':100,'weight_kg':weight}})
        assert r.status_code==201,r.text
        ids.append(r.json()['id'])
    assert client.put(path,json={**body,'birth':'2020-02-01','history':'updated','clinical':{'events':[]}}).status_code==200
    assert client.get(path).json()['history']=='updated'
    revisions=client.get(path+'/revisions').json()
    assert revisions[0]['snapshot']['clinical']['events'][0]['date']=='2021-02-03'
    exams=client.get('/api/v1/examinations?patient_id='+str(key)).json()
    assert [e['parameters']['weight_kg'] for e in exams]==[12,9]
    assert all(e['parameters']['age_days']==1461 for e in exams)
    assert client.get(path+'/reports').json()==[]
    client.post(f'/api/v1/cases/{ids[0]}/entries',json={'kind':'report','text':'Actual saved test report'})
    report=client.get(path+'/reports').json()[0]
    assert report['text']=='Actual saved test report' and report['review'] is None
    # Older clients omit structured fields: preserve existing clinical values and number.
    assert client.put(path,json={'name':'Old client','birth':'2020-01-01','sex':'unknown'}).status_code==200
    assert client.get(path).json()['business_number']=='TEST-001'
    assert client.get(path).json()['history']=='updated'
    for changes in [{'birth':'2099-01-01'},{'clinical':{'gestation_weeks':30,'gestation_days':7}},{'clinical':{'gestation_days':2}},{'clinical':{'events':[{'kind':'surgery','description':'x','date':'2019-01-01'}]}}]:
        assert client.post('/api/v1/patients',json={**body,'business_number':'TEST-INVALID',**changes}).status_code==422
    login('other')
    assert client.get(path).status_code==404
    assert client.get(path+'/reports').status_code==404
    assert client.get(path+'/revisions').status_code==404
    assert client.put(path,json=body).status_code==404
    login('reviewer')
    assert client.get(path).json()['can_edit'] is False
    assert client.put(path,json=body).status_code==403
    assert client.delete(path).status_code==405
