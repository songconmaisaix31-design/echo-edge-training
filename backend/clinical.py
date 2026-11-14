from __future__ import annotations
import json
import secrets
from datetime import date
from datetime import date as Date
from typing import Literal, Optional
from fastapi import Depends, HTTPException
from fastapi.responses import PlainTextResponse
from pydantic import BaseModel, Field, field_validator
from sqlalchemy import Column, ForeignKey, Integer, String, Text, select, update
from sqlalchemy.exc import IntegrityError
import platform_db as p

def business_number():
    return "EE-" + secrets.token_hex(6).upper()

class PatientRevision(p.Base):
    __tablename__ = "patient_revisions"
    id = Column(Integer, primary_key=True)
    patient_id = Column(ForeignKey("patients.id"), nullable=False, index=True)
    data = Column(Text, nullable=False)
    author = Column(ForeignKey("users.id"), nullable=False)
    created = Column(String(40), nullable=False)

class Patient(p.Base):
    __tablename__ = "patients"
    id = Column(Integer, primary_key=True)
    owner = Column(ForeignKey("users.id"), nullable=False)
    name = Column(String(120), nullable=False)
    birth = Column(String(10), nullable=False)
    sex = Column(String(16), nullable=False)
    notes = Column(Text, nullable=False)

    history = Column(Text, nullable=False, default='')
    treatment_history = Column(Text, nullable=False, default='')
    business_number = Column(String(40), unique=True, nullable=False, default=business_number)
    created = Column(String(40), nullable=True, default=lambda: p.now().isoformat()+"Z")
    clinical_data = Column(Text, nullable=False, default='{}')

class Examination(p.Base):
    __tablename__ = "examinations"
    id = Column(ForeignKey("cases.id"), primary_key=True)
    patient_id = Column(ForeignKey("patients.id"), nullable=False)
    snapshot = Column(Text, nullable=False)
    created = Column(String(40), nullable=False)
    kind = Column(String(80), nullable=False)
    reason = Column(Text, nullable=False)

class Entry(p.Base):
    __tablename__ = "clinical_entries"
    id = Column(Integer, primary_key=True)
    case_id = Column(ForeignKey("cases.id"), nullable=False)
    author = Column(ForeignKey("users.id"), nullable=False)
    kind = Column(String(24), nullable=False)
    text = Column(Text, nullable=False)
    state = Column(String(32), nullable=False)
    created = Column(String(40), nullable=False)

class MedicalEvent(BaseModel):
    kind: Literal["surgery", "intervention"]
    description: str = Field(min_length=1, max_length=2000)
    date: Optional[Date] = None

    @field_validator("date")
    @classmethod
    def event_date(cls, value):
        if value and value > date.today():
            raise ValueError("医疗事件日期不能晚于今天")
        return value

class PatientClinical(BaseModel):
    main_problem: str = Field("", max_length=4000)
    congenital_diagnosis: str = Field("", max_length=4000)
    other_diagnosis: str = Field("", max_length=4000)
    gestation_weeks: Optional[int] = Field(None, ge=0, le=45)
    gestation_days: Optional[int] = Field(None, ge=0, le=6)
    birth_weight_g: Optional[float] = Field(None, gt=0, le=10000, allow_inf_nan=False)
    events: list[MedicalEvent] = Field(default_factory=list, max_length=100)

class PatientInput(BaseModel):
    business_number: Optional[str] = Field(None, max_length=40, pattern=r"^[A-Za-z0-9-]+$")
    clinical: Optional[PatientClinical] = None
    name: str = Field(min_length=1, max_length=120)
    birth: date
    sex: Literal["unknown", "female", "male"]
    notes: str = Field(default="", max_length=4000)

    history: str = Field(default='', max_length=4000)
    treatment_history: str = Field(default='', max_length=4000)

    @field_validator("birth")
    @classmethod
    def valid_birth(cls, value):
        if value > date.today():
            raise ValueError("出生日期不得晚于今天")
        return value

    @field_validator("name")
    @classmethod
    def valid_name(cls, value):
        if not value.strip():
            raise ValueError("请填写姓名或脱敏标识")
        return value.strip()

class ExamInput(BaseModel):
    parameters: dict = Field(default_factory=dict)
    patient_id: int
    kind: str = Field(min_length=1, max_length=80)
    reason: str = Field(default="", max_length=4000)

class EntryInput(BaseModel):
    kind: Literal["review", "consultation", "report", "organization"]
    text: str = Field(min_length=1, max_length=12000)
    state: Literal["saved", "submitted", "needs_more", "feedback", "closed"] = "saved"

class NewUser(BaseModel):
    name: str = Field(min_length=1, max_length=80)
    password: str = Field(min_length=12, max_length=128)
    role: Literal["operator", "reviewer", "admin"]

class RoleInput(BaseModel):
    role: Literal["operator", "reviewer", "admin"]

def patient_access(db, key, user):
    item = db.get(Patient, key)
    if not item or (item.owner != user.id and user.role not in ("admin", "reviewer")):
        raise HTTPException(404, "患儿档案不存在")
    return item

def patient_json(item):
    return {**{field: getattr(item, field) for field in ("id", "name", "birth", "sex", "notes", "history", "treatment_history", "business_number", "created")}, "clinical": json.loads(item.clinical_data or '{}')}

def patient_values(body, existing=None):
    values = body.model_dump(mode="json", exclude_unset=existing is not None)
    clinical = values.pop("clinical", None)
    if clinical is not None:
        if clinical.get("gestation_days") is not None and clinical.get("gestation_weeks") is None:
            raise HTTPException(422, "填写胎龄天数时须填写周数")
        for event in clinical.get("events", []):
            if not event["description"].strip():
                raise HTTPException(422, "医疗事件描述不能为空")
            if event.get("date") and event["date"] < body.birth.isoformat():
                raise HTTPException(422, "医疗事件日期不能早于出生日期")
        values["clinical_data"] = json.dumps(clinical)
    number = values.get("business_number")
    if number:
        values["business_number"] = number.upper()
    else:
        values.pop("business_number", None)
    return values

def patient_commit(db):
    try:
        db.commit()
    except IntegrityError:
        db.rollback()
        raise HTTPException(409, "患儿业务编号已存在，请核对或更换编号")

def patient_revision(db, item, user):
    db.add(PatientRevision(patient_id=item.id, data=json.dumps(patient_json(item)), author=user.id, created=p.now().isoformat()+"Z"))

def register(app):
    @app.get("/api/v1/patients")
    def patients(q: str = "", user=Depends(p.current)):
        with p.Session() as db:
            query = select(Patient).order_by(Patient.id.desc())
            if user.role not in ("admin", "reviewer"):
                query = query.where(Patient.owner == user.id)
            items = db.scalars(query).all()
            return [patient_json(item) for item in items if q.lower() in (item.name + item.business_number + str(item.id)).lower()]

    @app.get("/api/v1/patients/{key}")
    def patient_detail(key: int, user=Depends(p.current)):
        with p.Session() as db:
            item = patient_access(db, key, user)
            return {**patient_json(item), "can_edit": item.owner == user.id or user.role == "admin"}

    @app.get("/api/v1/patients/{key}/reports")
    def patient_reports(key: int, user=Depends(p.current)):
        with p.Session() as db:
            patient_access(db, key, user)
            query = select(Entry, p.User.name).join(Examination, Entry.case_id == Examination.id).join(p.Case, p.Case.id == Examination.id).join(p.User, p.User.id == Entry.author).where(Examination.patient_id == key, Entry.kind == "report").order_by(Entry.id.desc())
            if user.role not in ("admin", "reviewer"):
                query = query.where(p.Case.owner == user.id)
            drafts = [{"id": e.id, "exam_id": e.case_id, "created": e.created, "state": e.state, "author": name, "text": e.text, "review": None} for e, name in db.execute(query)]
            from research_reports import ReportVersion, serialize_report
            issued = select(ReportVersion).join(Examination, Examination.id == ReportVersion.exam_id).join(
                p.Case, p.Case.id == Examination.id).where(Examination.patient_id == key)
            if user.role not in ("admin", "reviewer"):
                issued = issued.where(p.Case.owner == user.id)
            return [serialize_report(row) for row in db.scalars(issued.order_by(ReportVersion.id.desc()))] + drafts

    @app.get("/api/v1/patients/{key}/revisions")
    def patient_revisions(key: int, user=Depends(p.current)):
        with p.Session() as db:
            patient_access(db, key, user)
            return [{"id": v.id, "created": v.created, "author": v.author, "snapshot": json.loads(v.data)} for v in db.scalars(select(PatientRevision).where(PatientRevision.patient_id == key).order_by(PatientRevision.id.desc()))]

    @app.post("/api/v1/patients", status_code=201)
    def create_patient(body: PatientInput, user=Depends(p.current)):
        with p.Session() as db:
            item = Patient(owner=user.id, **patient_values(body))
            db.add(item)
            try:
                db.flush()
            except IntegrityError:
                db.rollback()
                raise HTTPException(409, "患儿业务编号已存在，请核对或更换编号")
            patient_revision(db, item, user)
            p.log(db, user, "patient.create", item.id)
            patient_commit(db)
            return patient_json(item)

    @app.put("/api/v1/patients/{key}")
    def edit_patient(key: int, body: PatientInput, user=Depends(p.current)):
        with p.Session() as db:
            item = patient_access(db, key, user)
            if item.owner != user.id and user.role != "admin":
                raise HTTPException(403, "只有创建者或管理员可编辑档案")
            patient_revision(db, item, user)
            for name, value in patient_values(body, item).items():
                setattr(item, name, value)
            p.log(db, user, "patient.edit", key)
            patient_commit(db)
            return patient_json(item)

    @app.get("/api/v1/examinations")
    def exams(patient_id: int = 0, user=Depends(p.current)):
        with p.Session() as db:
            query = select(Examination, p.Case).join(p.Case, Examination.id == p.Case.id)
            if user.role not in ("admin", "reviewer"):
                query = query.where(p.Case.owner == user.id)
            if patient_id:
                query = query.where(Examination.patient_id == patient_id)
            from exam_parameters import ClinicalVersion
            latest = {}
            for version in db.scalars(select(ClinicalVersion).join(Examination, ClinicalVersion.exam_id == Examination.id).join(p.Case, Examination.id == p.Case.id).where(p.Case.owner == user.id) if user.role not in ("admin", "reviewer") else select(ClinicalVersion)):
                if version.exam_id not in latest or version.id > latest[version.exam_id].id:
                    latest[version.exam_id] = version
            return [{**p.summary(case), "parameters": json.loads(latest[exam.id].data) if exam.id in latest else None, "patient_id": exam.patient_id, "snapshot": json.loads(exam.snapshot),
                     "created": exam.created, "kind": exam.kind, "reason": exam.reason}
                    for exam, case in db.execute(query.order_by(Examination.id.desc()))]

    @app.post("/api/v1/examinations", status_code=201)
    def create_exam(body: ExamInput, user=Depends(p.current)):
        with p.Session() as db:
            patient = patient_access(db, body.patient_id, user)
            case = p.Case(title=patient.name + " · " + body.kind, notes=body.reason, status="draft", owner=user.id)
            db.add(case)
            db.flush()
            db.add(Examination(id=case.id, patient_id=patient.id, snapshot=json.dumps(patient_json(patient)),
                               created=p.now().isoformat()+"Z", kind=body.kind, reason=body.reason))
            db.flush()
            if body.parameters:
                from exam_parameters import Parameters, append_parameters
                from pydantic import ValidationError
                try:
                    append_parameters(db, case.id, user, Parameters(**body.parameters))
                except ValidationError as error:
                    raise HTTPException(422, str(error))
            p.log(db, user, "examination.create", case.id)
            db.commit()
            return p.summary(case)

    @app.get("/api/v1/cases/{key}/entries")
    def entries(key: int, user=Depends(p.current)):
        with p.Session() as db:
            p.get_case(db, key, user)
            p.log(db, user, "case.access", key)
            rows = db.execute(select(Entry, p.User.name).join(p.User, Entry.author == p.User.id)
                              .where(Entry.case_id == key).order_by(Entry.id.desc())).all()
            db.commit()
            return [{"id": item.id, "kind": item.kind, "text": item.text, "state": item.state,
                     "created": item.created, "author": name} for item, name in rows]

    @app.post("/api/v1/cases/{key}/entries", status_code=201)
    def add_entry(key: int, body: EntryInput, user=Depends(p.current)):
        if not body.text.strip():
            raise HTTPException(422, "内容不能为空")
        if body.state in ("feedback", "needs_more") and user.role not in ("reviewer", "admin"):
            raise HTTPException(403, "专家反馈需要审核权限")
        if body.kind != "consultation" and body.state != "saved":
            raise HTTPException(422, "此记录类型不接受协作状态")
        with p.Session() as db:
            case = p.get_case(db, key, user)
            if user.role == "reviewer" and body.kind in ("report", "organization"):
                raise HTTPException(403, "此操作需要病例创建者或管理员权限")
            if body.kind == "consultation":
                previous = db.scalar(select(Entry).where(Entry.case_id == key, Entry.kind == "consultation").order_by(Entry.id.desc()))
                previous_state = previous.state if previous else None
                allowed = {None: {"submitted"}, "submitted": {"feedback", "needs_more"},
                           "needs_more": {"submitted", "closed"}, "feedback": {"submitted", "closed"}, "closed": {"submitted"}}
                if body.state not in allowed.get(previous_state, set()):
                    raise HTTPException(409, "协作状态转换无效，请刷新任务")
            item = Entry(case_id=key, author=user.id, **body.model_dump(), created=p.now().isoformat()+"Z")
            db.add(item)
            db.flush()
            p.log(db, user, body.kind+"."+body.state, key)
            db.commit()
            return {"id": item.id}

    @app.get("/api/v1/cases/{key}/report-export")
    def export(key: int, user=Depends(p.current)):
        with p.Session() as db:
            case = p.get_case(db, key, user)
            records = db.scalars(select(Entry).where(Entry.case_id == key).order_by(Entry.id)).all()
            lines = ["Echo Edge · 研究检查记录", case.title, "本报告仅包含已保存的研究检查记录，不自动纳入切面模型结果。"]
            for item in records:
                lines.extend([f"{item.created} | {item.kind} | {item.state}", item.text])
            p.log(db, user, "report.export", key)
            db.commit()
            return PlainTextResponse("\n\n".join(lines), headers={"Content-Disposition": f'attachment; filename="examination-{key}.txt"', "Cache-Control": "no-store"})

    @app.get("/api/v1/users")
    def users(user=Depends(p.current)):
        if user.role != "admin":
            raise HTTPException(403, "用户管理需要管理员权限")
        with p.Session() as db:
            return [{"id": item.id, "name": item.name, "role": item.role} for item in db.scalars(select(p.User))]

    @app.post("/api/v1/users", status_code=201)
    def create_user(body: NewUser, user=Depends(p.current)):
        if user.role != "admin":
            raise HTTPException(403, "需要管理员权限")
        salt = secrets.token_hex(16)
        with p.Session() as db:
            item = p.User(name=body.name.strip(), role=body.role, password=salt+":"+p.digest(body.password, salt))
            if not item.name:
                raise HTTPException(422, "用户名不能为空")
            db.add(item)
            try:
                db.flush()
                p.log(db, user, "user.create", item.id)
                db.commit()
            except IntegrityError:
                db.rollback()
                raise HTTPException(409, "用户名已存在")
            return {"id": item.id}

    @app.put("/api/v1/users/{key}/role")
    def role(key: int, body: RoleInput, user=Depends(p.current)):
        if user.role != "admin" or key == user.id:
            raise HTTPException(403, "需要其他管理员修改角色")
        with p.Session() as db:
            item = db.get(p.User, key)
            if not item:
                raise HTTPException(404, "用户不存在")
            item.role = body.role
            for login in db.scalars(select(p.Login).where(p.Login.user_id == key)):
                db.delete(login)
            p.log(db, user, "user.role", key)
            db.commit()
            return {"id": key, "role": body.role}



