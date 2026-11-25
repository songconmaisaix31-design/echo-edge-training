import json
import math
from datetime import date
from typing import Literal

import platform_db as p
from clinical import Examination
from fastapi import Depends, HTTPException
from pydantic import BaseModel, Field, model_validator
from sqlalchemy import Column, ForeignKey, Integer, String, Text, select


class ClinicalVersion(p.Base):
    __tablename__ = "examination_parameters"
    id = Column(Integer, primary_key=True)
    exam_id = Column(ForeignKey("examinations.id"), nullable=False, index=True)
    data = Column(Text, nullable=False)
    author = Column(ForeignKey("users.id"), nullable=False)
    created = Column(String(40), nullable=False)

class Measurement(p.Base):
    __tablename__ = "ultrasound_measurements"
    id = Column(Integer, primary_key=True)
    exam_id = Column(ForeignKey("examinations.id"), nullable=False, index=True)
    data = Column(Text, nullable=False)
    author = Column(ForeignKey("users.id"), nullable=False)
    created = Column(String(40), nullable=False)

class Parameters(BaseModel):
    exam_date: date
    height_cm: float | None = Field(None, gt=0, le=300, allow_inf_nan=False)
    weight_kg: float | None = Field(None, gt=0, le=500, allow_inf_nan=False)
    heart_rate: float | None = Field(None, gt=0, le=400, allow_inf_nan=False)
    systolic: float | None = Field(None, gt=0, le=400, allow_inf_nan=False)
    diastolic: float | None = Field(None, gt=0, le=300, allow_inf_nan=False)
    spo2: float | None = Field(None, ge=0, le=100, allow_inf_nan=False)
    clinical_notes: str = Field("", max_length=4000)
    indication: str = Field("", max_length=4000)
    doctor: str = Field("", max_length=120)
    device: str = Field("", max_length=120)

    @model_validator(mode="after")
    def validate_values(self):
        if self.exam_date > date.today():
            raise ValueError("检查日期不能晚于今天")
        if (self.systolic is None) != (self.diastolic is None):
            raise ValueError("血压须同时填写收缩压和舒张压，或全部留空")
        if self.systolic is not None and self.diastolic > self.systolic:
            raise ValueError("舒张压不能大于收缩压")
        return self

class MeasurementInput(BaseModel):
    replaces_id: int | None = Field(None, gt=0)
    item: str = Field(min_length=1, max_length=120)
    value: float = Field(allow_inf_nan=False)
    unit: str = Field(min_length=1, max_length=40)
    view: str = Field("", max_length=120)
    phase: str = Field("", max_length=120)
    source: Literal["manual", "ai"] = "manual"
    media_id: str | None = None
    frame_index: int | None = Field(None, ge=0)
    review: Literal["pending", "confirmed", "uncertain"] = "pending"

def checked_exam(db, key, user, write=False):
    case = p.get_case(db, key, user)
    exam = db.get(Examination, key)
    if not exam:
        raise HTTPException(404, "该记录尚未关联独立检查")
    if write and case.owner != user.id and user.role != "admin":
        raise HTTPException(403, "需要检查创建者或管理员权限")
    return exam

def parameters_data(body, exam):
    birth = date.fromisoformat(json.loads(exam.snapshot)["birth"])
    if body.exam_date < birth:
        raise HTTPException(422, "检查日期不能早于出生日期")
    data = body.model_dump(mode="json")
    data["age_days"] = (body.exam_date-birth).days
    data["bsa_m2"] = math.sqrt(body.height_cm*body.weight_kg/3600) if body.height_cm is not None and body.weight_kg is not None else None
    data["bsa_formula"] = "Mosteller"
    return data

def append_parameters(db, key, user, body):
    exam = checked_exam(db, key, user, True)
    data = parameters_data(body, exam)
    db.add(ClinicalVersion(exam_id=key, data=json.dumps(data), author=user.id, created=p.now().isoformat()+"Z"))
    p.log(db, user, "examination.parameters", key)
    return data

def register(app):
    @app.get("/api/v1/examinations/{key}/parameters")
    def versions(key: int, user=Depends(p.current)):
        with p.Session() as db:
            checked_exam(db,key,user)
            return [{"id":v.id,"created":v.created,"author":v.author,**json.loads(v.data)} for v in db.scalars(select(ClinicalVersion).where(ClinicalVersion.exam_id==key).order_by(ClinicalVersion.id.desc()))]

    @app.post("/api/v1/examinations/{key}/parameters")
    def save(key: int, body: Parameters, user=Depends(p.current)):
        with p.Session() as db:
            data=append_parameters(db,key,user,body)
            db.commit()
            return data

    @app.get("/api/v1/examinations/{key}/measurements")
    def measurements(key: int, user=Depends(p.current)):
        with p.Session() as db:
            checked_exam(db,key,user)
            return [{"id":v.id,"created":v.created,"author":v.author,**json.loads(v.data)} for v in db.scalars(select(Measurement).where(Measurement.exam_id==key).order_by(Measurement.id.desc()))]

    @app.post("/api/v1/examinations/{key}/measurements",status_code=201)
    def measure(key: int, body: MeasurementInput, user=Depends(p.current)):
        if body.source=="ai":
            raise HTTPException(409, "模型未接入，不能录入未经模型产生的 AI 测量")
        if not body.item.strip() or not body.unit.strip():
            raise HTTPException(422, "测量项目和单位不能为空")
        with p.Session() as db:
            checked_exam(db,key,user,True)
            if body.replaces_id is not None:
                previous = db.get(Measurement, body.replaces_id)
                if not previous or previous.exam_id != key:
                    raise HTTPException(422, "原测量不属于本次检查")
            if body.frame_index is not None and not body.media_id:
                raise HTTPException(422,"关键帧必须关联影像")
            if body.media_id:
                media=db.get(p.Media,body.media_id)
                if not media or media.case_id!=key:
                    raise HTTPException(422,"影像不属于本次检查")
                if body.frame_index is not None:
                    from main import ROOT
                    metadata=json.loads((ROOT/body.media_id/"metadata.json").read_text(encoding="utf-8"))
                    if body.frame_index>=metadata["frameCount"]:
                        raise HTTPException(422,"关键帧索引超出范围")
            item=Measurement(exam_id=key,data=body.model_dump_json(),author=user.id,created=p.now().isoformat()+"Z")
            db.add(item)
            p.log(db,user,"measurement.create",key)
            db.commit()
            return {"id":item.id}

    @app.get("/api/v1/examinations/{key}/z-score")
    def zscore(key: int,user=Depends(p.current)):
        with p.Session() as db:
            checked_exam(db,key,user)
        return {"status":"reference_not_configured","value":None,"message":"尚未配置经临床确认的参考公式"}

