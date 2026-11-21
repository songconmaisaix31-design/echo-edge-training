import json
import subprocess
import sys
import unittest
from dataclasses import replace
from pathlib import Path
from tempfile import TemporaryDirectory

from openpyxl import load_workbook

from train_echo import HEADERS, check_site_splits, clip_entry


ROOT = Path(__file__).resolve().parent
TEMPLATE = ROOT / "templates" / "儿童心超标注模板.xlsx"
POCUS_CASE = "10.24908/pocus.v6i1.14755"
POCUS_CENTER = "Alberta Children's Hospital"
CASE_SERIES = "10.1016/j.case.2021.08.002"
CASE_SERIES_CENTER = "Children's Healthcare of Atlanta"


def registered_video(clip_id, patient_id, filename, site, split):
    return {"clip_id": clip_id, "patient_id": patient_id, "study_id": patient_id,
            "relative_path": filename, "mode": "2D", "site_code": site, "split": split}


def alberta_case(first, second):
    # 期刊 S1、S3 来自同一次就诊，病例代号取自文章 DOI。
    return [registered_video("S1", POCUS_CASE, "Damji-et-al-Video-S1-PSLX-1.mp4", POCUS_CENTER, first),
            registered_video("S3", POCUS_CASE, "Damji-et-al-Video-S3-A4C-TV-Endo-1.mp4", POCUS_CENTER, second)]


def atlanta_cases(first, second):
    # 期刊 Video 1、2 分属病例 1、2。路径为补充视频在验收清单内的别名。
    return [registered_video(f"Video-{number}", f"{CASE_SERIES}:case-{number}",
                             f"Nelson-Video-{number}.mp4", CASE_SERIES_CENTER, split)
            for number, split in enumerate((first, second), 1)]


class SplitPreflightTests(unittest.TestCase):
    def check_command(self, rows, expected, train_command=False):
        work = ROOT / "work"
        work.mkdir(exist_ok=True)
        with TemporaryDirectory(prefix="split-preflight-", dir=work) as folder:
            scratch = Path(folder)
            workbook = scratch / "inventory.xlsx"
            report_path = scratch / "report.json"
            book = load_workbook(TEMPLATE)
            try:
                sheet = book["视频清单"]
                for number, row in enumerate(rows, 5):
                    for column, header in enumerate(HEADERS["视频清单"], 1):
                        sheet.cell(row=number, column=column, value=row.get(header))
                book.save(workbook)
            finally:
                book.close()
            command = [sys.executable, "-X", "utf8", str(ROOT / "train_echo.py"),
                       "train" if train_command else "preflight",
                       "--labels", str(workbook), "--media", str(scratch),
                       "--cache-dir", str(scratch / "cache"), "--report", str(report_path)]
            if train_command:
                command.extend(["--run-dir", str(scratch / "run"), "--preflight-only"])
            result = subprocess.run(command, cwd=ROOT, capture_output=True, encoding="utf-8", timeout=60)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            report = json.loads(report_path.read_text(encoding="utf-8"))
            self.assertEqual(report["status"], "blocked")
            self.assertEqual(report["files"], [])
            self.assertEqual(report["intervals"], [])
            self.assertFalse((scratch / "run").exists())
        diagnostics = {issue["code"]: issue for issue in report["issues"]}
        self.assertEqual(diagnostics.keys() - {"too_few_labels", "no_confirmed_labels"}, expected)
        for code in expected:
            self.assertEqual(diagnostics[code]["severity"], "error")
            self.assertEqual(diagnostics[code]["sheet"], "视频清单")
        return diagnostics

    def test_same_child_in_train_and_test(self):
        rows = alberta_case("train", "test")
        diagnostics = self.check_command(rows, {"patient_cross_split", "site_cross_split"})
        for code in ("patient_cross_split", "site_cross_split"):
            self.assertEqual(diagnostics[code]["related_rows"], [5, 6])
            self.assertEqual([item["split"] for item in diagnostics[code]["related_files"]], ["train", "test"])

    def test_same_center_with_distinct_children(self):
        diagnostics = self.check_command(atlanta_cases("val", "test"), {"site_cross_split"}, train_command=True)
        issue = diagnostics["site_cross_split"]
        self.assertEqual(issue["site_code"], CASE_SERIES_CENTER)
        self.assertEqual([item["split"] for item in issue["related_files"]], ["val", "test"])
        self.assertEqual(issue["related_rows"], [5, 6])

    def test_assigned_video_without_center_code(self):
        for site in (None, "   "):
            with self.subTest(site=site):
                rows = alberta_case("train", "train")
                rows[1]["site_code"] = site
                diagnostics = self.check_command(rows, {"missing_site_code"})
                self.assertEqual(diagnostics["missing_site_code"]["row"], 6)

    def test_resolved_roles_include_rows_without_explicit_split(self):
        rows = atlanta_cases("", "")
        videos = []
        for number, record in enumerate(rows, 5):
            values = {field: record.get(field) for field in HEADERS["视频清单"]}
            clip_id, _, clip = clip_entry({**values, "_row": number}, "视频清单")
            videos.append((clip_id, clip))
        first, second = (clip.patient for _, clip in videos)
        for inherited in (False, True):
            with self.subTest(inherited=inherited):
                selected = videos.copy()
                if inherited:
                    clip_id, clip = selected[0]
                    selected[0] = clip_id, replace(clip, split="train")
                issues = []

                def issue(code, message, **details):
                    issues.append((code, details))

                self.assertFalse(check_site_splits(selected, {first: "train", second: "train"}, issue))
                self.assertEqual(issues, [])
                self.assertTrue(check_site_splits(selected, {first: "train", second: "val"}, issue))
                [(code, details)] = issues
                self.assertEqual(code, "site_cross_split")
                self.assertEqual([row["split"] for row in details["related_files"]], ["train", "val"])


if __name__ == "__main__":
    unittest.main()
