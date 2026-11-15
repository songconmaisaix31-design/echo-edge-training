"""设备面板同步接口。

ESP32 面板没有登录态, 也不能被要求先建立一个会话; 它需要的是一个可以按固定
间隔拉取的、稳定的"当前应显示什么"快照。这里只做读取, 不产生分析任务,
也不复用 /analysis-jobs 那套需要用户鉴权的私有资源。

字段名刻意取短: 设备端是手写 JSON 扫描, 每个字节都要经过 115200 的串口日志
和 1KB 帧缓冲, 长字段名只会让解析和调试都变慢。
"""
import json
import os
from datetime import datetime

from fastapi import Header, HTTPException
from sqlalchemy import select

import platform_db as db
from analysis_jobs import AnalysisJob

VIEW_MODEL = "stanford_view"
STRUCTURE_MODEL = "cardiacudc_fourchamber"

# 屏上只放得下这几个字, 完整名称留在文档和前端里
LABELS = {"A4C": "四腔心", "PSAX": "左室短轴"}

# 超过这个时长没有新结果, 约定为"旧数据", 屏上要明确降级显示
STALE_SECONDS = 900


def _authorize(supplied):
    """未配置密钥时放开(局域网自用), 配置了就必须带对。"""
    expected = os.environ.get("ECHO_PANEL_KEY", "")
    if not expected:
        return
    if supplied != expected:
        raise HTTPException(401, "面板密钥不匹配")


def _latest(session, model_type):
    query = (select(AnalysisJob)
             .where(AnalysisJob.model_type == model_type)
             .order_by(AnalysisJob.id.desc())
             .limit(1))
    return session.scalars(query).first()


def _age_seconds(stamp):
    if stamp is None:
        return None
    return max(0, int((datetime.utcnow() - stamp).total_seconds()))


def _view_payload(job):
    try:
        output = json.loads(job.output) if job.output else None
    except ValueError:
        return None
    if not isinstance(output, dict):
        return None
    view = output.get("view")
    scores = output.get("probabilities")
    if view not in LABELS and view is not None:
        return None
    if not isinstance(scores, dict):
        return None
    a4c = scores.get("A4C")
    psax = scores.get("PSAX")
    if not all(isinstance(v, (int, float)) for v in (a4c, psax)):
        return None
    confidence = max(a4c, psax) if view else 0.0
    return {
        "view": view,
        "label": LABELS.get(view, "待判定"),
        "confidence": round(float(confidence), 4),
        "a4c": round(float(a4c), 4),
        "psax": round(float(psax), 4),
        "frames": output.get("sampled_frames"),
        "precision": output.get("precision"),
    }


def register(app):
    @app.get("/api/v1/panel/state")
    def panel_state(x_panel_key: str = Header(default=None)):
        """最近一次切面推理结果的设备快照。

        没有成功结果时返回 state=empty, 设备应显示"待分析"而不是报错 —— 空状态
        是正常状态, 不是故障。
        """
        _authorize(x_panel_key)
        with db.Session() as session:
            job = _latest(session, VIEW_MODEL)
            running = session.scalars(
                select(AnalysisJob)
                .where(AnalysisJob.model_type == VIEW_MODEL,
                       AnalysisJob.state == "running")
                .order_by(AnalysisJob.id.desc()).limit(1)
            ).first()

            body = {
                "type": "view",
                "state": "empty",
                "seq": 0,
                "ageS": None,
                "case": None,
                "view": None,
                "label": None,
                "confidence": 0.0,
                "a4c": 0.0,
                "psax": 0.0,
                "frames": None,
                "precision": None,
                "serverS": int(datetime.now().timestamp()),
            }

            if running is not None and (job is None or running.id > job.id):
                body["state"] = "running"
                body["case"] = running.exam_id
                return body

            if job is None:
                return body

            if job.state != "succeeded":
                body["state"] = "failed"
                body["seq"] = job.id
                body["case"] = job.exam_id
                body["ageS"] = _age_seconds(job.completed or job.created)
                return body

            payload = _view_payload(job)
            if payload is None:
                body["state"] = "failed"
                body["seq"] = job.id
                body["case"] = job.exam_id
                body["ageS"] = _age_seconds(job.completed or job.created)
                return body

            age = _age_seconds(job.completed or job.created)
            body.update(payload)
            body.update({
                "state": "stale" if age is not None and age > STALE_SECONDS else "ready",
                "seq": job.id,
                "case": job.exam_id,
                "ageS": age,
                "model": job.model_version,
            })
            return body

    @app.get("/api/v1/panel/health")
    def panel_health():
        """设备开机自检用: 只要能应答就说明网络到后端这一段通了。"""
        return {"ok": True, "serverS": int(datetime.now().timestamp())}
