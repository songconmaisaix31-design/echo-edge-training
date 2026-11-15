import json
import re
import sqlite3
import sys


# 通过 SSH 标准输入迁移操作员的既有加盐摘要，不读取病例或会话。
account = json.load(sys.stdin)
name = account["name"]
password_hash = account["password_hash"]
if not isinstance(name, str) or not 1 <= len(name) <= 80:
    raise ValueError("账号名称无效")
if not isinstance(password_hash, str) or not re.fullmatch(r"[0-9a-f]{32}:[0-9a-f]{64}", password_hash):
    raise ValueError("账号密码摘要无效")
with sqlite3.connect("/opt/echo-edge/data/platform.db") as database:
    database.execute("INSERT INTO users (name, password, role) VALUES (?, ?, ?)",
                     (name, password_hash, "operator"))
print("已创建操作员：" + name)
