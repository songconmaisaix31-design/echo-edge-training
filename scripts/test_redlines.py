import json
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory
import unittest


ROOT = Path(__file__).resolve().parents[1]
EF_REPORT = {
    "kind": "project",
    "release": "stanford-ef-epoch5",
    "report": "https://github.com/songconmaisaix31-design/echo-edge-training/releases/download/"
              "stanford-ef-epoch5/Stanford-EF-test.json",
}


class RedlineTests(unittest.TestCase):
    def scan(self, name, text, reports=None, claims=(), exceptions=()):
        work = ROOT / "work"
        work.mkdir(exist_ok=True)
        with TemporaryDirectory(prefix="redlines-", dir=work) as folder:
            scratch = Path(folder)
            rules = scratch / "rules"
            rules.mkdir()
            (rules / "banned-claims.txt").write_bytes((ROOT / "rules/banned-claims.txt").read_bytes())
            (rules / "metrics.json").write_text(json.dumps({
                "reports": reports or {}, "claims": claims, "exceptions": exceptions,
            }, ensure_ascii=False), encoding="utf-8")
            document = scratch / name
            document.parent.mkdir(parents=True, exist_ok=True)
            document.write_text(text, encoding="utf-8")
            return subprocess.run(
                [sys.executable, "-X", "utf8", str(ROOT / "scripts/check_redlines.py"), "--root", str(scratch)],
                capture_output=True, encoding="utf-8", timeout=15,
            )

    def test_banned_claim_in_frontend_and_report_template(self):
        for name in ("frontend/src/Review.tsx", "backend/templates/review.txt", "ROADMAP.md"):
            with self.subTest(path=name):
                result = self.scan(name, "保证确诊\n")
                self.assertEqual(result.returncode, 1)
                self.assertIn(f"{name}:1: banned_claim", result.stdout)

    def test_unregistered_metrics_in_prose_and_both_table_orientations(self):
        for text in (
            "患儿级 MAE 为 3.80。\n",
            "患儿级 MAE 为\n3.80。\n",
            "| 模型 | Dice |\n| --- | --- |\n| LV | **0.9047** |\n",
            "| 指标 | 结果 |\n| --- | --- |\n| AUROC | 0.996 |\n",
        ):
            with self.subTest(text=text):
                result = self.scan("docs/results.md", text)
                self.assertEqual(result.returncode, 1)
                self.assertIn("unregistered_metric", result.stdout)

    def test_registered_claim_only_applies_to_exact_text_and_location(self):
        name = "docs/results.md"
        claim = "患儿级 MAE 3.80。"
        claims = [{"path": name, "text": claim, "reports": ["ef"]}]
        valid = self.scan(name, claim, {"ef": EF_REPORT}, claims)
        wrong = self.scan(name, "患儿级 MAE 4.69。", {"ef": EF_REPORT}, claims)
        self.assertEqual(valid.returncode, 0, valid.stdout + valid.stderr)
        self.assertEqual(wrong.returncode, 1)
        self.assertIn("unregistered_metric", wrong.stdout)

    def test_table_headers_are_bound_to_the_registry(self):
        name = "docs/results.md"
        row = "| 患儿级 | 3.80 |"
        claims = [{"path": name, "text": row, "headers": ["单位", "MAE"], "reports": ["ef"]}]
        result = self.scan(name, "| 单位 | AUROC |\n| --- | --- |\n" + row, {"ef": EF_REPORT}, claims)
        self.assertEqual(result.returncode, 1)
        self.assertIn("unregistered_metric", result.stdout)

    def test_expired_exception_fails(self):
        result = self.scan("README.md", "保证确诊", exceptions=[{
            "path": "README.md", "text": "保证确诊", "rule": "banned_claim",
            "reason": "保留原审阅记录", "expires": "2000-01-01",
        }])
        self.assertEqual(result.returncode, 1)
        self.assertIn("例外已到期", result.stdout)

    def test_ef_directional_qualifier_required(self):
        name = "docs/EF.md"
        claim = "EF AUROC 0.996。"
        result = self.scan(name, claim, {"ef": EF_REPORT},
                           [{"path": name, "text": claim, "reports": ["ef"]}])
        self.assertEqual(result.returncode, 1)
        self.assertIn("ef_directional_only", result.stdout)
        claim = "EF AUROC 0.996，5 例阳性，仅作方向性参考。"
        result = self.scan(name, claim, {"ef": EF_REPORT},
                           [{"path": name, "text": claim, "reports": ["ef"]}])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_third_party_table_must_be_labelled_and_separate(self):
        name = "docs/results.md"
        first = "| 项目 EF | 3.80 |"
        second = "| 第三方论文 | 3.80 |"
        headers = ["来源", "MAE"]
        claims = [{"path": name, "text": first, "headers": headers, "reports": ["ef"]},
                  {"path": name, "text": second, "headers": headers, "reports": ["paper"]}]
        reports = {"ef": EF_REPORT, "paper": {"kind": "third_party", "report": "https://doi.org/10.1016/j.echo.2023.01.015"}}
        table = "| 来源 | MAE |\n| --- | --- |\n"
        result = self.scan(name, table + first + "\n" + second, reports, claims)
        self.assertEqual(result.returncode, 1)
        self.assertIn("mixed_metric_sources", result.stdout)
        unlabelled = "| 论文 | 3.80 |"
        result = self.scan(name, table + unlabelled, reports,
                           [{"path": name, "text": unlabelled, "headers": headers, "reports": ["paper"]}])
        self.assertEqual(result.returncode, 1)
        self.assertIn("third_party_label", result.stdout)
