import json
import os
from pathlib import Path
from shutil import copyfile
import subprocess
import sys
from tempfile import TemporaryDirectory
import unittest

from evaluate_multiview import PUBLIC_CLIPS, PUBLIC_SOURCE, public_frames
from multiview_study import Exam, Frame, heldout_exams, metric_rates, outcome_counts


ROOT = Path(__file__).resolve().parents[1]


class IndependentEvaluationTests(unittest.TestCase):
    def setUp(self):
        work = ROOT / "work"
        work.mkdir(exist_ok=True)
        folder = TemporaryDirectory(prefix="multiview-evaluation-", dir=work)
        self.addCleanup(folder.cleanup)
        self.scratch = Path(folder.name)
        self.report = self.scratch / "report.json"
        self.env = os.environ.copy()
        self.env.pop("ECHO_MULTIVIEW_REVIEW_ROOT", None)

    def run_cli(self, *arguments):
        return subprocess.run([sys.executable, "-X", "utf8", str(ROOT / "evaluate_multiview.py"),
                               *arguments, "--report", str(self.report)],
                              cwd=ROOT, env=self.env, capture_output=True, encoding="utf-8", timeout=30)

    def new_review_directory(self):
        root = self.scratch / "restricted"
        study = root / "review"
        study.mkdir(parents=True)
        self.env["ECHO_MULTIVIEW_REVIEW_ROOT"] = str(root)
        return study

    def test_unconfigured_governance_blocks_before_creating_output(self):
        result = self.run_cli("evaluate", "--study", "review")
        self.assertEqual(result.returncode, 1)
        self.assertIn("protocol_approval_missing", result.stdout)
        self.assertIn("data_authorization_missing", result.stdout)
        self.assertFalse(self.report.exists())

    def test_missing_authorization_is_reported_before_data_read(self):
        study = self.new_review_directory()
        copyfile(ROOT / "docs/MULTIVIEW_EVALUATION.md", study / "protocol-approval.json")
        result = self.run_cli("evaluate", "--study", "review")
        self.assertEqual(result.returncode, 1)
        self.assertIn("data_authorization_missing", result.stdout)
        self.assertNotIn("cohort.json", result.stdout)
        self.assertFalse(self.report.exists())

    def test_missing_protocol_is_reported_before_data_read(self):
        study = self.new_review_directory()
        (study / "data-authorization.json").write_text(json.dumps({"source": PUBLIC_SOURCE}), encoding="utf-8")
        result = self.run_cli("evaluate", "--study", "review")
        self.assertEqual(result.returncode, 1)
        self.assertIn("protocol_approval_missing", result.stdout)
        self.assertNotIn("cohort.json", result.stdout)
        self.assertFalse(self.report.exists())

    def test_review_draft_and_public_citation_do_not_grant_clinical_access(self):
        study = self.new_review_directory()
        copyfile(ROOT / "docs/MULTIVIEW_EVALUATION.md", study / "protocol-approval.json")
        (study / "data-authorization.json").write_text(json.dumps({"source": PUBLIC_SOURCE}), encoding="utf-8")
        result = self.run_cli("evaluate", "--study", "review")
        self.assertEqual(result.returncode, 1)
        self.assertIn("protocol_not_approved", result.stdout)
        self.assertNotIn("cohort.json", result.stdout)
        self.assertFalse(self.report.exists())

    def test_study_cannot_select_paths_outside_review_root(self):
        self.new_review_directory()
        result = self.run_cli("evaluate", "--study", "../review")
        self.assertEqual(result.returncode, 1)
        self.assertIn("invalid_study_id", result.stdout)
        self.assertFalse(self.report.exists())

    def test_missing_public_bundle_fails_without_creating_report(self):
        result = self.run_cli("verify-public", "--media-dir", str(self.scratch))
        self.assertEqual(result.returncode, 1)
        self.assertFalse(self.report.exists())

    def public_directory(self):
        path = os.environ.get("ECHO_VERIFY_MULTIVIEW_DIR")
        if not path:
            self.skipTest("需要 ECHO_VERIFY_MULTIVIEW_DIR 指向登记的 S1/S3 原始视频")
        return Path(path).resolve(strict=True)

    def test_public_workflow_uses_real_video_and_keeps_report_when_repeated(self):
        directory = self.public_directory()
        result = self.run_cli("verify-public", "--media-dir", str(directory))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        original = self.report.read_bytes()
        report = json.loads(original)
        self.assertEqual(report["scope"], "public_workflow_verification")
        self.assertEqual(report["source"], PUBLIC_SOURCE)
        self.assertNotIn("results", report)
        self.assertEqual(report["verified_cases"], 2)
        complete, partial = (report["scenarios"][key] for key in ("both_views", "plax_withheld"))
        self.assertEqual(complete["coverage"], {"A4C": "covered", "PLAX": "covered"})
        self.assertEqual({frame["media_id"] for frame in complete["confirmed_frames"]}, {"S1", "S3"})
        self.assertEqual(partial["coverage"], {"A4C": "covered", "PLAX": "missing"})
        self.assertEqual(partial["state"], "insufficient_evidence")
        self.assertEqual(partial["insufficient_evidence"]["reason"], "coverage")
        repeated = self.run_cli("verify-public", "--media-dir", str(directory))
        self.assertEqual(repeated.returncode, 1)
        self.assertEqual(self.report.read_bytes(), original)

    def test_public_bundle_rejects_swapped_original(self):
        directory = self.public_directory()
        first, second = (clip[2] for clip in PUBLIC_CLIPS)
        copyfile(directory / second, self.scratch / first)
        result = self.run_cli("verify-public", "--media-dir", str(self.scratch))
        self.assertEqual(result.returncode, 1)
        self.assertIn("public_sample_digest_mismatch", result.stdout)
        self.assertFalse(self.report.exists())

    def public_exam(self):
        observations = public_frames(self.public_directory())
        timelines = {frame["media_id"]: offsets for frame, offsets in observations}
        return Exam(exam_id="10.24908/pocus.v6i1.14755", patient_id="10.24908/pocus.v6i1.14755",
                    site_code="Alberta Children's Hospital", enrollment_site="Alberta Children's Hospital",
                    split="test", age_band="儿童", device="SonoSite", adequacy="not_assessed",
                    planned_signs=[], required_views=["A4C", "PLAX"], media=timelines,
                    confirmed_frames=[frame for frame, _ in observations])

    def test_real_public_frames_reject_out_of_bounds_evidence(self):
        exam = self.public_exam()
        exam.check_frame(Frame(media_id="S1", frame_index=0, timestamp_ms=0))
        with self.assertRaisesRegex(ValueError, "unlinked_or_out_of_bounds_frame"):
            exam.check_frame(Frame(media_id="S1", frame_index=len(exam.media["S1"]), timestamp_ms=0))
        with self.assertRaisesRegex(ValueError, "frame_timestamp_mismatch"):
            exam.check_frame(Frame(media_id="S1", frame_index=0, timestamp_ms=1))

    def test_public_exam_without_clinical_results_cannot_produce_determinate_metrics(self):
        exam = self.public_exam()
        with self.assertRaisesRegex(ValueError, "missing_train_val_test_allocation"):
            heldout_exams([exam])
        measures = metric_rates(outcome_counts([(exam, "missing_reference", "missing_result")]))
        self.assertEqual(measures["missing_result"], {"numerator": 1, "denominator": 1, "value": 1.0})
        self.assertIsNone(measures["sensitivity"]["value"])
        self.assertIsNone(measures["specificity"]["value"])

    def test_empty_reference_and_prediction_denominators_are_not_estimated(self):
        measures = metric_rates(outcome_counts([]))
        for measure in measures.values():
            self.assertEqual(measure, {"numerator": 0, "denominator": 0, "value": None})


if __name__ == "__main__":
    unittest.main()
