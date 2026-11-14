import json
import sys

from fastapi import HTTPException
import pytest

import model_inference


def test_cut_view_adapter(tmp_path, monkeypatch):
    script = tmp_path / "infer.py"
    report = tmp_path / "report.json"
    engine = tmp_path / "model.engine"
    video = tmp_path / "source.mp4"
    script.write_text(
        "import json,sys\n"
        "assert sys.argv[1] == 'predict' and sys.argv[2] == '--engine-report'\n"
        "assert sys.argv[4] == '--video' and sys.argv[5].endswith('source.mp4')\n"
        "assert sys.argv[6:] == ['--precision', 'fp16']\n"
        "print(json.dumps(dict(precision='fp16', view=None, sampled_frames=2, "
        "probabilities=dict(A4C=0.6, PSAX=0.4))))\n",
        encoding="utf-8",
    )
    report.write_text(json.dumps({"models": {"fp16": {"file": engine.name}}}), encoding="utf-8")
    monkeypatch.setenv("ECHO_INFERENCE_PYTHON", sys.executable)
    monkeypatch.setenv("ECHO_INFERENCE_SCRIPT", str(script))
    monkeypatch.setenv("ECHO_ENGINE_REPORT", str(report))
    with pytest.raises(HTTPException) as missing_model:
        model_inference.predict(tmp_path)
    assert missing_model.value.status_code == 503
    engine.touch()
    with pytest.raises(HTTPException) as missing_video:
        model_inference.predict(tmp_path)
    assert missing_video.value.status_code == 422
    video.touch()
    result = model_inference.predict(tmp_path)
    assert result["view"] is None and result["sampled_frames"] == 2
    assert result["probabilities"] == {"A4C": 0.6, "PSAX": 0.4}
    script.write_text("print('invalid output')\n", encoding="utf-8")
    with pytest.raises(HTTPException) as invalid:
        model_inference.predict(tmp_path)
    assert invalid.value.status_code == 502
