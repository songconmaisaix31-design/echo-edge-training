import os
import secrets
import sys
from pathlib import Path

from sqlalchemy import select

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "backend"))

from platform_db import Session, User, digest

salt = secrets.token_hex(16)
name = os.environ["ECHO_OPERATOR_NAME"]
password = f"{salt}:{digest(os.environ['ECHO_OPERATOR_PASSWORD'], salt)}"
with Session.begin() as session:
    user = session.scalar(select(User).where(User.name == name))
    if user is None:
        session.add(User(name=name, role="operator", password=password))
    elif user.role == "operator":
        user.password = password
    else:
        raise RuntimeError("本机操作账号的角色须为 operator。")
