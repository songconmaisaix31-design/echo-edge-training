import hashlib
import hmac
import os
import secrets
from datetime import datetime, timedelta, timezone

from fastapi import Depends, HTTPException, Request
from pydantic import BaseModel, Field
from sqlalchemy import Column, DateTime, ForeignKey, Integer, String, Text, create_engine, select
from sqlalchemy.orm import declarative_base, sessionmaker

Base = declarative_base()
engine = create_engine(os.environ.get("ECHO_DATABASE_URL", "sqlite:///./echo.db"))
Session = sessionmaker(engine, expire_on_commit=False)

def now():
    return datetime.now(timezone.utc).replace(tzinfo=None)

class User(Base):
    __tablename__ = "users"
    id = Column(Integer, primary_key=True)
    name = Column(String(80), unique=True, nullable=False)
    password = Column(String(150), nullable=False)
    role = Column(String(20), nullable=False)

class Login(Base):
    __tablename__ = "sessions"
    token = Column(String(64), primary_key=True)
    user_id = Column(ForeignKey("users.id"), nullable=False)
    expires = Column(DateTime, nullable=False)

class Case(Base):
    __tablename__ = "cases"
    id = Column(Integer, primary_key=True)
    title = Column(String(120), nullable=False)
    notes = Column(Text, nullable=False)
    status = Column(String(20), nullable=False)
    owner = Column(ForeignKey("users.id"), nullable=False)

class Media(Base):
    __tablename__ = "media"
    id = Column(String(32), primary_key=True)
    case_id = Column(ForeignKey("cases.id"), nullable=False, index=True)

class Review(Base):
    __tablename__ = "reviews"
    id = Column(Integer, primary_key=True)
    case_id = Column(ForeignKey("cases.id"), nullable=False, index=True)
    author = Column(ForeignKey("users.id"), nullable=False)
    text = Column(Text, nullable=False)
    created = Column(DateTime, nullable=False)

class Audit(Base):
    __tablename__ = "audit"
    id = Column(Integer, primary_key=True)
    actor = Column(ForeignKey("users.id"), nullable=False)
    action = Column(String(80), nullable=False)
    target = Column(String(80), nullable=False)
    created = Column(DateTime, nullable=False)

def log(db, user, action, target):
    db.add(Audit(actor=user.id, action=action, target=str(target), created=now()))

def digest(password, salt):
    return hashlib.pbkdf2_hmac("sha256", password.encode(), bytes.fromhex(salt), 600000).hex()

def token_hash(token):
    return hashlib.sha256(token.encode()).hexdigest()

def current(request: Request):
    token = request.cookies.get("echo_session", "")
    with Session() as db:
        login = db.get(Login, token_hash(token))
        if not login or login.expires <= now():
            raise HTTPException(401, "请先登录")
        return db.get(User, login.user_id)

def get_case(db, key, user):
    case = db.get(Case, key)
    if not case or (case.owner != user.id and user.role not in ("reviewer", "admin")):
        raise HTTPException(404, "病例不存在")
    return case

def protect_media(identifier, user):
    with Session() as db:
        media = db.get(Media, identifier)
        if not media:
            raise HTTPException(404, "影像不存在")
        get_case(db, media.case_id, user)

class Credentials(BaseModel):
    name: str = Field(min_length=1, max_length=80)
    password: str = Field(min_length=1, max_length=128)

class CaseInput(BaseModel):
    title: str = Field(min_length=1, max_length=120)
    notes: str = Field(default="", max_length=4000)

class ReviewInput(BaseModel):
    text: str = Field(min_length=1, max_length=8000)

def summary(case):
    return {"id": case.id, "title": case.title, "notes": case.notes, "status": case.status}

def register(app):
    from fastapi import Response

    @app.post("/api/v1/login")
    def login(body: Credentials, response: Response):
        with Session() as db:
            user = db.scalar(select(User).where(User.name == body.name))
            salt, expected = user.password.split(":") if user else ("00"*16, "00"*32)
            if not hmac.compare_digest(digest(body.password, salt), expected) or not user:
                raise HTTPException(401, "用户名或密码错误")
            token = secrets.token_urlsafe(32)
            db.add(Login(token=token_hash(token), user_id=user.id, expires=now()+timedelta(hours=8)))
            log(db, user, "login", user.id)
            db.commit()
            response.set_cookie("echo_session", token, httponly=True, samesite="strict",
                                secure=os.environ.get("ECHO_SECURE_COOKIE")=="1", max_age=28800)
            return {"name":user.name,"role":user.role}

    @app.post("/api/v1/logout")
    def logout(request: Request, response: Response, user=Depends(current)):
        with Session() as db:
            db.delete(db.get(Login, token_hash(request.cookies["echo_session"])))
            log(db,user,"logout",user.id)
            db.commit()
        response.delete_cookie("echo_session")
        return {"ok":True}

    @app.get("/api/v1/me")
    def me(user=Depends(current)):
        return {"name":user.name,"role":user.role}

    @app.get("/api/v1/cases")
    def cases(q: str="", status: str="", user=Depends(current)):
        with Session() as db:
            query=select(Case).order_by(Case.id.desc())
            if user.role == "operator":
                query=query.where(Case.owner==user.id)
            if q:
                query=query.where(Case.title.contains(q, autoescape=True))
            if status:
                query=query.where(Case.status==status)
            return [summary(case) for case in db.scalars(query.limit(200))]

    @app.post("/api/v1/cases", status_code=201)
    def create(body: CaseInput, user=Depends(current)):
        with Session() as db:
            case=Case(**body.model_dump(),owner=user.id,status="draft")
            db.add(case)
            db.flush()
            log(db,user,"case.create",case.id)
            db.commit()
            return summary(case)

    @app.put("/api/v1/cases/{key}")
    def update(key: int, body: CaseInput, user=Depends(current)):
        with Session() as db:
            case=get_case(db,key,user)
            if case.owner!=user.id and user.role!="admin":
                raise HTTPException(403,"只有创建者或管理员可以编辑")
            case.title,case.notes=body.title,body.notes
            log(db,user,"case.update",key)
            db.commit()
            return summary(case)

    @app.get("/api/v1/cases/{key}")
    def detail(key: int, user=Depends(current)):
        with Session() as db:
            case=get_case(db,key,user)
            reviews=db.execute(select(Review,User.name).join(User,Review.author==User.id)
                               .where(Review.case_id==key).order_by(Review.id))
            return {**summary(case),
                    "media":[m.id for m in db.scalars(select(Media).where(Media.case_id==key))],
                    "reviews":[{"text":r.text,"author":name,"created":r.created.isoformat()+"Z"} for r,name in reviews]}

    @app.post("/api/v1/cases/{key}/submit")
    def submit(key: int,user=Depends(current)):
        with Session() as db:
            case=get_case(db,key,user)
            if case.owner!=user.id and user.role!="admin":
                raise HTTPException(403,"仅病例创建者或管理员可提交")
            if case.status!="draft" or not db.scalar(select(Media.id).where(Media.case_id==key).limit(1)):
                raise HTTPException(409,"只有含影像的草稿可提交")
            case.status="submitted"
            log(db,user,"case.submit",key)
            db.commit()
            return summary(case)

    @app.post("/api/v1/cases/{key}/reviews")
    def review(key: int,body: ReviewInput,user=Depends(current)):
        if user.role not in ("reviewer","admin"):
            raise HTTPException(403,"需要审核权限")
        with Session() as db:
            case=get_case(db,key,user)
            if case.status not in ("submitted","reviewed") or not body.text.strip():
                raise HTTPException(409,"请先提交病例并填写审核意见")
            db.add(Review(case_id=key,author=user.id,text=body.text,created=now()))
            case.status="reviewed"
            log(db,user,"case.review",key)
            db.commit()
            return summary(case)

    @app.get("/api/v1/cases/{key}/report")
    def report(key: int,user=Depends(current)):
        from fastapi.responses import PlainTextResponse
        case=detail(key,user)
        lines=["Echo Edge · 研究影像报告",f"病例 {key} · {case['title']}",f"状态 {case['status']}",
               f"影像 {len(case['media'])} 段","模型未部署，未执行切面、分割、质量或异常分析。","人工审核"]
        lines.extend(f"{r['created']} | {r['author']} | {r['text']}" for r in case["reviews"])
        if not case["reviews"]:
            lines.append("待人工审核")
        with Session() as db:
            log(db,user,"report.export",key)
            db.commit()
        return PlainTextResponse("\n".join(lines),headers={"Content-Disposition":f'attachment; filename="case-{key}.txt"',"Cache-Control":"no-store"})

    @app.get("/api/v1/audit")
    def audit(user=Depends(current)):
        if user.role!="admin":
            raise HTTPException(403,"需要管理员权限")
        with Session() as db:
            return [{"actor":a.actor,"action":a.action,"target":a.target,"created":a.created} for a in db.scalars(select(Audit).order_by(Audit.id.desc()).limit(100))]

