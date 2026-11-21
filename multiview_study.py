from collections import Counter, defaultdict
from datetime import date
import math
from pathlib import Path
import random
from statistics import quantiles
from typing import Annotated, Generic, Literal, TypeVar

from pydantic import BaseModel, ConfigDict, Field, StringConstraints, ValidationError, model_validator

from backend.multiview_coverage import View, view_coverage


Text = Annotated[str, StringConstraints(strip_whitespace=True, min_length=1)]
Sign = Literal["SEPTAL_CONTOUR_INTERRUPTION", "VALVE_FOCAL_ECHO"]
Split = Literal["train", "val", "test"]
ReferenceState = Literal["present", "absent", "indeterminate", "not_assessed"]
PredictionState = Literal["present", "absent", "insufficient_evidence", "failed"]
REFERENCES = ("present", "absent", "indeterminate", "not_assessed", "missing_reference")
PREDICTIONS = ("present", "absent", "insufficient_evidence", "failed", "missing_result")
VERSION = "0.2"
ANALYSIS_VERSION = "1"


class Record(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)


class SampleFloor(Record):
    positive: int = Field(ge=1)
    negative: int = Field(ge=1)


class StudyApproval(Record):
    status: Literal["approved"]
    record_id: Text
    protocol_version: Literal["0.2"]
    dictionary_version: Literal["0.2"]
    approved_on: date
    clinical_signoff_id: Text
    statistics_signoff_id: Text
    reference_review_signoff_id: Text
    model_version: Text
    threshold_version: Text
    analysis_version: Literal["1"]
    signs: dict[Sign, SampleFloor] = Field(min_length=1)
    age_bands: list[Text] = Field(min_length=1)
    device_groups: list[Text] = Field(min_length=1)
    bootstrap_seed: int
    bootstrap_repetitions: int = Field(ge=200, le=10000)
    minimum_centers: int = Field(ge=2)


class DataAuthorization(Record):
    status: Literal["authorized"]
    record_id: Text
    purpose: Literal["independent_multiview_evaluation"]
    protocol_approval_id: Text
    cohort_id: Text
    cohort_freeze_id: Text
    references_freeze_id: Text
    predictions_freeze_id: Text
    effective_on: date
    expires_on: date


class GovernanceError(ValueError):
    pass


def governance(study: Path, today: date):
    protocol_path = study / "protocol-approval.json"
    authorization_path = study / "data-authorization.json"
    missing = [code for path, code in (
        (protocol_path, "protocol_approval_missing"),
        (authorization_path, "data_authorization_missing"),
    ) if not path.is_file()]
    if missing:
        raise GovernanceError(", ".join(missing))
    if any(path.resolve().parent != study for path in (protocol_path, authorization_path)):
        raise GovernanceError("governance_path_outside_study")
    try:
        approval = StudyApproval.model_validate_json(protocol_path.read_bytes())
    except ValidationError:
        raise GovernanceError("protocol_not_approved") from None
    try:
        authorization = DataAuthorization.model_validate_json(authorization_path.read_bytes())
    except ValidationError:
        raise GovernanceError("data_not_authorized") from None
    if approval.record_id != authorization.protocol_approval_id:
        raise GovernanceError("authorization_protocol_mismatch")
    if not approval.approved_on <= authorization.effective_on <= today <= authorization.expires_on:
        raise GovernanceError("authorization_outside_validity")
    return approval, authorization


class Frame(Record):
    media_id: Text
    frame_index: int = Field(ge=0)
    timestamp_ms: float = Field(ge=0, allow_inf_nan=False)

    @property
    def identity(self):
        return self.media_id, self.frame_index


class ConfirmedFrame(Frame):
    view: View


class Exam(Record):
    exam_id: Text
    patient_id: Text
    site_code: Text
    enrollment_site: Text
    split: Split
    age_band: Text
    device: Text
    adequacy: Literal["adequate", "limited", "uninterpretable", "not_assessed"]
    planned_signs: list[Sign]
    required_views: list[View]
    media: dict[Text, list[Annotated[float, Field(ge=0, allow_inf_nan=False)]]]
    confirmed_frames: list[ConfirmedFrame]

    def check_frame(self, frame):
        offsets = self.media.get(frame.media_id)
        if offsets is None or frame.frame_index >= len(offsets):
            raise ValueError("unlinked_or_out_of_bounds_frame")
        if not math.isclose(offsets[frame.frame_index], frame.timestamp_ms, abs_tol=1e-6, rel_tol=0):
            raise ValueError("frame_timestamp_mismatch")

    @model_validator(mode="after")
    def validate_evidence(self):
        if len(self.planned_signs) != len(set(self.planned_signs)):
            raise ValueError("duplicate_planned_sign")
        if self.split != "test" and self.planned_signs:
            raise ValueError("planned_sign_outside_test_split")
        if self.planned_signs and not self.required_views:
            raise ValueError("missing_required_views_for_planned_exam")
        if len(self.required_views) != len(set(self.required_views)):
            raise ValueError("duplicate_required_view")
        for offsets in self.media.values():
            if not offsets or offsets[0] != 0 or offsets != sorted(offsets):
                raise ValueError("invalid_source_frame_timeline")
        if len({frame.identity for frame in self.confirmed_frames}) != len(self.confirmed_frames):
            raise ValueError("duplicate_confirmed_frame")
        for frame in self.confirmed_frames:
            self.check_frame(frame)
        return self


class EvidenceRow(Record):
    exam_id: Text
    sign_code: Sign
    supporting: list[Frame]
    opposing: list[Frame]
    missing_views: list[View]

    @property
    def key(self):
        return self.exam_id, self.sign_code

    @model_validator(mode="after")
    def validate_evidence(self):
        frames = self.supporting + self.opposing
        if len({frame.identity for frame in frames}) != len(frames):
            raise ValueError("duplicate_or_conflicting_evidence_frame")
        if len(self.missing_views) != len(set(self.missing_views)):
            raise ValueError("duplicate_missing_view")
        return self


class Reference(EvidenceRow):
    state: ReferenceState
    adequacy: Literal["adequate", "limited", "uninterpretable", "not_assessed"]
    adjudication_id: Text
    reason: Text | None = None

    @model_validator(mode="after")
    def validate_result(self):
        if self.state == "present" and (not self.supporting or self.adequacy in ("uninterpretable", "not_assessed")):
            raise ValueError("missing_reference_support")
        if self.state == "absent" and (self.adequacy != "adequate" or not self.opposing or self.missing_views):
            raise ValueError("missing_reference_absence_evidence")
        if self.state in ("indeterminate", "not_assessed") and not self.reason:
            raise ValueError("missing_reference_reason")
        return self


class Prediction(EvidenceRow):
    state: PredictionState
    reason: Literal["coverage", "quality", "out_of_scope", "execution"] | None = None

    @model_validator(mode="after")
    def validate_result(self):
        if self.state == "present" and not self.supporting:
            raise ValueError("missing_prediction_support")
        if self.state == "absent" and not self.opposing:
            raise ValueError("missing_prediction_absence_evidence")
        if self.state == "insufficient_evidence" and self.reason not in {"coverage", "quality", "out_of_scope"}:
            raise ValueError("invalid_insufficient_evidence_reason")
        if self.state == "failed" and self.reason != "execution":
            raise ValueError("missing_execution_failure_reason")
        if self.state in ("present", "absent") and self.reason is not None:
            raise ValueError("unexpected_prediction_reason")
        return self


Row = TypeVar("Row")


class FrozenData(Record, Generic[Row]):
    cohort_id: Text
    protocol_version: Literal["0.2"]
    dictionary_version: Literal["0.2"]
    freeze_id: Text
    rows: list[Row]


class FrozenPredictions(FrozenData[Prediction]):
    model_version: Text
    threshold_version: Text


def read_frozen(path, schema, cohort_id, freeze_id):
    if path.resolve().parent != path.parent:
        raise ValueError("data_path_outside_authorized_study")
    frozen = schema.model_validate_json(path.read_bytes())
    if (frozen.cohort_id, frozen.freeze_id) != (cohort_id, freeze_id):
        raise ValueError("unauthorized_cohort_or_freeze")
    return frozen


def heldout_exams(inventory):
    exams, patients, centers, enrollment = {}, {}, {}, {}
    media_exam = {}
    patient_sites = defaultdict(set)
    for exam in inventory:
        if exam.exam_id in exams:
            raise ValueError("duplicate_exam_id")
        if patients.setdefault(exam.patient_id, exam.split) != exam.split:
            raise ValueError("patient_cross_split")
        if centers.setdefault(exam.site_code, exam.split) != exam.split:
            raise ValueError("site_cross_split")
        if enrollment.setdefault(exam.patient_id, exam.enrollment_site) != exam.enrollment_site:
            raise ValueError("inconsistent_patient_enrollment_site")
        for media_id in exam.media:
            if media_exam.setdefault(media_id, exam.exam_id) != exam.exam_id:
                raise ValueError("media_shared_by_exams")
        patient_sites[exam.patient_id].add(exam.site_code)
        exams[exam.exam_id] = exam
    if set(patients.values()) != {"train", "val", "test"}:
        raise ValueError("missing_train_val_test_allocation")
    if any(site not in patient_sites[patient] for patient, site in enrollment.items()):
        raise ValueError("invalid_patient_enrollment_site")
    return {key: exam for key, exam in exams.items() if exam.split == "test"}


def evidence_index(records, planned):
    indexed = {}
    for record in records:
        if record.key not in planned:
            raise ValueError("result_outside_test_plan")
        if record.key in indexed:
            raise ValueError("duplicate_exam_sign_result")
        exam = planned[record.key]
        if set(record.missing_views) - set(exam.required_views):
            raise ValueError("unplanned_missing_view")
        requires_coverage = record.state == "absent" or (isinstance(record, Prediction) and record.state == "present")
        if requires_coverage and (record.missing_views or "missing" in coverage_report(exam).values()):
            raise ValueError("determinate_result_without_required_view")
        for frame in record.supporting + record.opposing:
            exam.check_frame(frame)
        indexed[record.key] = record
    return indexed


def rate(num, den):
    return {"numerator": num, "denominator": den, "value": num / den if den else None}


def outcome_counts(rows):
    return Counter((reference, prediction) for _, reference, prediction in rows)


def metric_rates(counts):
    positive, negative = (sum(counts[reference, predicted] for predicted in PREDICTIONS)
                          for reference in ("present", "absent"))
    true_positive = counts["present", "present"]
    true_negative = counts["absent", "absent"]
    verdicts = sum(counts[reference, predicted] for reference in REFERENCES for predicted in ("present", "absent"))
    determinate_verdicts = true_positive + counts["present", "absent"] + true_negative + counts["absent", "present"]
    total = sum(counts.values())
    return {
        "sensitivity": rate(true_positive, positive),
        "specificity": rate(true_negative, negative),
        "conditional_sensitivity": rate(true_positive, true_positive + counts["present", "absent"]),
        "conditional_specificity": rate(true_negative, true_negative + counts["absent", "present"]),
        "coverage": rate(verdicts, total),
        "reference_determinate_coverage": rate(determinate_verdicts, positive + negative),
        **{state: rate(sum(counts[reference, state] for reference in REFERENCES), total)
           for state in PREDICTIONS[2:]},
    }


def bootstrap(rows, approval, eligible):
    samples = {name: [] for name in ("sensitivity", "specificity")}
    grouped = defaultdict(lambda: defaultdict(list))
    for row in rows:
        exam, _, _ = row
        grouped[exam.enrollment_site][exam.patient_id].append(row)
    centers = [[patients[key] for key in sorted(patients)] for _, patients in sorted(grouped.items())]
    if eligible and len(centers) >= approval.minimum_centers:
        randomizer = random.Random(approval.bootstrap_seed)
        for _ in range(approval.bootstrap_repetitions):
            drawn = [row for center in randomizer.choices(centers, k=len(centers))
                     for patient in randomizer.choices(center, k=len(center)) for row in patient]
            measures = metric_rates(outcome_counts(drawn))
            for name in samples:
                if measures[name]["value"] is not None:
                    samples[name].append(measures[name]["value"])
    intervals = {}
    for name, observed in samples.items():
        enough = len(observed) >= math.ceil(0.95 * approval.bootstrap_repetitions)
        limits = quantiles(observed, n=40, method="inclusive") if enough else []
        intervals[name] = {"interval": [limits[0], limits[-1]] if limits else None, "valid_repetitions": len(observed)}
    return intervals


def summarize(rows, floor):
    counts = outcome_counts(rows)
    measures = metric_rates(counts)
    eligible = measures["sensitivity"]["denominator"] >= floor.positive and (
        measures["specificity"]["denominator"] >= floor.negative)
    for name, minimum in (("sensitivity", floor.positive), ("conditional_sensitivity", floor.positive),
                          ("specificity", floor.negative), ("conditional_specificity", floor.negative)):
        if measures[name]["denominator"] < minimum:
            measures[name]["value"] = None
    return {
        "planned_exams": len(rows),
        "reference_prediction_counts": {
            reference: {prediction: counts[reference, prediction] for prediction in PREDICTIONS}
            for reference in REFERENCES
        },
        "rates": measures,
        "sample_floor_met": eligible,
    }


def coverage_report(exam):
    return view_coverage(exam.required_views, (frame.view for frame in exam.confirmed_frames))


def score(planned, references, predictions, approval):
    results = {}
    for sign, floor in approval.signs.items():
        rows = []
        subgroups = defaultdict(list)
        frame_counts = {stance: Counter() for stance in ("supporting", "opposing")}
        covered_views = required_views = 0
        for key in sorted(key for key in planned if key[1] == sign):
            exam = planned[key]
            reference = references.get(key)
            prediction = predictions.get(key)
            row = (exam, reference.state if reference else "missing_reference",
                   prediction.state if prediction else "missing_result")
            rows.append(row)
            view_states = coverage_report(exam)
            covered = list(view_states.values()).count("covered")
            covered_views += covered
            required_views += len(view_states)
            for field, value in (("age_band", exam.age_band), ("site_code", exam.site_code),
                                 ("device", exam.device), ("adequacy", exam.adequacy), ("covered_views", str(covered))):
                subgroups[field, value].append(row)
            if row[1] in ("present", "absent") and row[2] in ("present", "absent"):
                for stance, counts in frame_counts.items():
                    expected = {frame.identity for frame in getattr(reference, stance)}
                    observed = {frame.identity for frame in getattr(prediction, stance)}
                    counts["observed"] += len(observed)
                    counts["matched"] += len(expected & observed)
        result = summarize(rows, floor)
        result["bootstrap_95"] = bootstrap(rows, approval, result["sample_floor_met"])
        for stance, counts in frame_counts.items():
            result[f"{stance}_frame_agreement"] = rate(counts["matched"], counts["observed"])
        result["required_view_coverage"] = rate(covered_views, required_views)
        result["exploratory_subgroups"] = [
            {"field": field, "value": value, **summarize(items, floor)}
            for (field, value), items in sorted(subgroups.items())
        ]
        results[sign] = result
    return results


def evaluate(study, approval, authorization):
    cohort_id = authorization.cohort_id
    allocation = read_frozen(study / "cohort.json", FrozenData[Exam], cohort_id, authorization.cohort_freeze_id)
    exams = heldout_exams(allocation.rows)
    planned = {(key, sign): exam for key, exam in exams.items() for sign in exam.planned_signs}
    if any(sign not in approval.signs for _, sign in planned):
        raise ValueError("unapproved_planned_sign")
    if any(exam.age_band not in approval.age_bands or exam.device not in approval.device_groups for exam in planned.values()):
        raise ValueError("unapproved_subgroup")
    references = read_frozen(study / "references.json", FrozenData[Reference], cohort_id, authorization.references_freeze_id)
    predictions = read_frozen(study / "predictions.json", FrozenPredictions, cohort_id, authorization.predictions_freeze_id)
    if (predictions.model_version, predictions.threshold_version) != (approval.model_version, approval.threshold_version):
        raise ValueError("unapproved_model_or_threshold_version")
    return {
        "scope": "independent_multiview_evaluation",
        "protocol_version": VERSION,
        "dictionary_version": VERSION,
        "analysis_version": ANALYSIS_VERSION,
        "protocol_approval_id": approval.record_id,
        "data_authorization_id": authorization.record_id,
        "cohort_id": authorization.cohort_id,
        "model_version": approval.model_version,
        "threshold_version": approval.threshold_version,
        "freeze_ids": {"cohort": allocation.freeze_id, "references": references.freeze_id,
                       "predictions": predictions.freeze_id},
        "statistical_plan": approval.model_dump(include={"signs", "age_bands", "device_groups", "bootstrap_seed",
                                                         "bootstrap_repetitions", "minimum_centers"}),
        "results": score(planned, evidence_index(references.rows, planned), evidence_index(predictions.rows, planned), approval),
    }
