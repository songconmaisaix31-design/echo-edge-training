import os
import secrets
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "backend"))

from platform_db import Session, User, digest

salt = secrets.token_hex(16)
password = digest(os.environ["ECHO_VERIFY_PASSWORD"], salt)
with Session.begin() as session:
    session.add(User(name=os.environ["ECHO_VERIFY_USER"], role="operator", password=f"{salt}:{password}"))
