import json
import os
from pathlib import Path
import secrets
import sys


configuration = json.load(sys.stdin)
key = configuration["review_key"]
if not isinstance(key, str) or not key or any(character in key for character in "\r\n\x00"):
    raise ValueError("DeepSeek 凭据无效")

values = {
    "ECHO_DATABASE_URL": "sqlite:////opt/echo-edge/data/platform.db",
    "ECHO_MEDIA_ROOT": "/opt/echo-edge/data/media",
    "ECHO_INFERENCE_PYTHON": "/opt/echo-edge/venvs/model/bin/python",
    "ECHO_STRUCTURE_PYTHON": "/opt/echo-edge/venvs/model/bin/python",
    "ECHO_INFERENCE_SCRIPT": "/opt/echo-edge/src/infer_stanford.py",
    "ECHO_ENGINE_REPORT": "/opt/echo-edge/models/Stanford-TensorRT-FP32-FP16-test.json",
    "ECHO_STANFORD_CHECKPOINT": "/opt/echo-edge/models/Stanford-A4C-PSAX-best.pt",
    "ECHO_CARDIACUDC_CHECKPOINT": "/opt/echo-edge/models/cardiacudc-fourchamber-v1/CardiacUDC-FourChamber-UNet-epoch18.pt",
    "ECHO_SECURE_COOKIE": "1",
    "ECHO_REVIEW_BASE_URL": "https://api.deepseek.com",
    "ECHO_REVIEW_MODEL": "deepseek-flash",
    "ECHO_REVIEW_API_KEY_ENV": "ANTHROPIC_AUTH_TOKEN",
    "ANTHROPIC_AUTH_TOKEN": key,
    "ECHO_PANEL_KEY": secrets.token_urlsafe(32),
}
destination = Path("/etc/echo-edge.env")
descriptor = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
with os.fdopen(descriptor, "w", encoding="utf-8") as output:
    for name, value in values.items():
        output.write(f"{name}={json.dumps(value)}\n")
print("已创建工作站环境文件，权限 0600。")
