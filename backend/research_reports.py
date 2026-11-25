import json

import platform_db as db
from analysis_contract import evidence_shortfall
from analysis_jobs import AnalysisJob
from clinical import Examination
from fastapi import Depends, HTTPException
from fastapi.responses import PlainTextResponse
from pydantic import BaseModel, Field
from sqlalchemy import Column, DateTime, ForeignKey, Integer, Text, select


class ReportVersion(db.Base):
    __tablename__ = "research_report_versions"

    id = Column(Integer, primary_key=True)
    exam_id = Column(ForeignKey("examinations.id"), nullable=False, index=True)
    review_id = Column(ForeignKey("reviews.id"), nullable=False)
    author = Column(ForeignKey("users.id"), nullable=False)
    created = Column(DateTime, nullable=False)
    snapshot = Column(Text, nullable=False)


class IssueInput(BaseModel):
    review_id: int
    text: str = Field(min_length=1, max_length=12000)
    analysis_ids: list[int] = Field(default_factory=list, max_length=50)


def serialize_report(row):
    return {"id": row.id, "version": row.id, "exam_id": row.exam_id,
            "created": row.created.isoformat()+"Z", "state": "issued",
            **json.loads(row.snapshot)}


def review_versions(session, key):
    query = select(db.Review, db.User.name).join(db.User, db.User.id == db.Review.author)
    return [{"id": row.id, "version": row.id, "author": name, "author_id": row.author,
             "created": row.created.isoformat()+"Z", "text": row.text}
            for row, name in session.execute(query.where(db.Review.case_id == key)
                                             .order_by(db.Review.id.desc()))]


def register(app):
    @app.get("/api/v1/examinations/{key}/formal-reviews")
    def reviews(key: int, user=Depends(db.current)):
        with db.Session() as session:
            db.get_case(session, key, user)
            return review_versions(session, key)

    @app.get("/api/v1/examinations/{key}/report-versions")
    def versions(key: int, user=Depends(db.current)):
        with db.Session() as session:
            db.get_case(session, key, user)
            query = select(ReportVersion).where(ReportVersion.exam_id == key)
            return [serialize_report(row) for row in session.scalars(query.order_by(ReportVersion.id.desc()))]

    @app.post("/api/v1/examinations/{key}/report-versions", status_code=201)
    def issue(key: int, body: IssueInput, user=Depends(db.current)):
        if user.role not in ("reviewer", "admin"):
            raise HTTPException(403, "签发研究报告需要审核权限")
        if not body.text.strip():
            raise HTTPException(422, "报告正文不能为空")
        with db.Session.begin() as session:
            case = db.get_case(session, key, user)
            if session.get(Examination, key) is None or case.status != "reviewed":
                raise HTTPException(409, "正式检查须完成审核后才能签发")
            review = next((r for r in review_versions(session, key) if r["id"] == body.review_id), None)
            if review is None:
                raise HTTPException(422, "审核版本不属于本次检查")
            evidence = []
            for job_id in dict.fromkeys(body.analysis_ids):
                job = session.get(AnalysisJob, job_id)
                if (job is None or job.exam_id != key or job.state != "succeeded"
                        or job.model_type != "stanford_view" or not job.output):
                    raise HTTPException(422, "只能引用本次检查的成功切面分析")
                output = json.loads(job.output)
                if evidence_shortfall(job.model_type, output) is not None:
                    raise HTTPException(422, "证据不足的切面分析不能作为报告依据")
                evidence.append({"id": job.id, "media_id": job.media_id,
                                 "model_version": job.model_version, "output": output})
            snapshot = {"author": user.name, "text": body.text.strip(),
                        "review": review, "analysis": evidence}
            row = ReportVersion(exam_id=key, review_id=body.review_id, author=user.id,
                                created=db.now(), snapshot=json.dumps(snapshot, ensure_ascii=False))
            session.add(row)
            session.flush()
            db.log(session, user, "report.issue", row.id)
            return serialize_report(row)

    @app.get("/api/v1/examinations/{key}/report-versions/{version}/export")
    def export(key: int, version: int, user=Depends(db.current)):
        with db.Session.begin() as session:
            db.get_case(session, key, user)
            row = session.get(ReportVersion, version)
            if row is None or row.exam_id != key:
                raise HTTPException(404, "报告版本不存在")
            report = serialize_report(row)
            review = report["review"]
            lines = ["Cardio bridge · 已签发研究报告", f"检查 #{key} · 报告版本 #{version}",
                     f"签发：{report['author']} · {report['created']}", report["text"],
                     f"审核版本 #{review['version']} · {review['author']} · {review['created']}",
                     review["text"], "引用的 AI 切面分析（研究参考）"]
            for job in report["analysis"]:
                lines.append(f"任务 #{job['id']} · 影像 {job['media_id']} · {job['model_version']}")
                lines.append(json.dumps(job["output"], ensure_ascii=False))
            db.log(session, user, "report.export_version", version)
            return PlainTextResponse("\n\n".join(lines), headers={
                "Content-Disposition": f'attachment; filename="exam-{key}-v{version}.txt"',
                "Cache-Control": "no-store",
            })
