import argparse
import getpass
import secrets

from platform_db import Session, User, digest

parser = argparse.ArgumentParser()
parser.add_argument("name")
parser.add_argument("role", choices=["admin", "operator", "reviewer"])
args = parser.parse_args()
password = getpass.getpass("Password (12+ characters): ")
if len(password) < 12:
    parser.error("Password too short")
salt = secrets.token_hex(16)
with Session() as db:
    db.add(User(name=args.name, role=args.role, password=salt + ":" + digest(password, salt)))
    db.commit()
