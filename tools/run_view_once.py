import os
import sys
import time

ROOT = r"D:\Codex\echo-edge-2026-11-15\work"
E2E = os.path.join(ROOT, "integration-e2e")
BACKEND = os.path.join(ROOT, "echo-edge-settings", "backend")

os.environ.update({
    "ECHO_API_PYTHON": os.path.join(E2E, "api-venv", "Scripts", "python.exe"),
    "ECHO_INFERENCE_PYTHON": os.path.join(E2E, "deployment-check", "model-venv", "Scripts", "python.exe"),
    "ECHO_STRUCTURE_PYTHON": os.path.join(E2E, "deployment-check", "model-venv", "Scripts", "python.exe"),
    "ECHO_INFERENCE_SCRIPT": os.path.join(ROOT, "echo-edge-integration", "infer_stanford.py"),
    "ECHO_ENGINE_REPORT": os.path.join(E2E, "models", "Stanford-TensorRT-FP32-FP16-test.json"),
    "ECHO_STANFORD_CHECKPOINT": os.path.join(E2E, "models", "Stanford-A4C-PSAX-best.pt"),
    "ECHO_CARDIACUDC_CHECKPOINT": os.path.join(
        E2E, "models", "cardiacudc-fourchamber-v1", "CardiacUDC-FourChamber-UNet-epoch18.pt"),
    "ECHO_DATABASE_URL": "sqlite:///" + os.path.join(E2E, "integration.db").replace("\\", "/"),
    "ECHO_MEDIA_ROOT": os.path.join(E2E, "media"),
})

sys.path.insert(0, BACKEND)

import platform_db as db            # noqa: E402
import analysis_jobs                # noqa: E402
from pathlib import Path            # noqa: E402

media_id = sys.argv[1] if len(sys.argv) > 1 else "473b0b90b7dbf782c0ec8dcb619334e9"
folder = Path(os.environ["ECHO_MEDIA_ROOT"]) / media_id

with db.Session() as s:
    user = s.get(db.User, 1)
    if user is None:
        raise SystemExit("库里没有 user id=1")

t0 = time.time()
out = analysis_jobs.run(media_id, folder, user, legacy=True)
print("elapsed %.1fs" % (time.time() - t0))
print("view=%s  probs=%s  frames=%s  precision=%s"
      % (out.get("view"), out.get("probabilities"), out.get("sampled_frames"), out.get("precision")))
