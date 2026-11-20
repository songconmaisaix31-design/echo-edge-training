"""阶段二的单元测试：划分、评分、校准、区间。

    python -m pytest test_emb_head.py -q
"""

from __future__ import annotations

import random

import numpy as np

from train_emb_head import (bootstrap_ci, fit_calibration, score,
                            split_by_subject)


def make_entries(n_videos=20, n_subjects=10):
    entries = []
    for index in range(n_videos):
        entries.append({"key": f"k{index:03d}", "path": None,
                        "subject": f"P{index % n_subjects:03d}",
                        "ef": 20.0 + index, "split": "test" if index % 5 == 0 else "train"})
    return entries


def test_subject_split_has_no_overlap():
    train, val, test = split_by_subject(make_entries(), 0.2, seed=0)
    train_subjects = {e["subject"] for e in train}
    val_subjects = {e["subject"] for e in val}
    test_subjects = {e["subject"] for e in test}
    assert not train_subjects & val_subjects
    assert not train_subjects & test_subjects
    assert not val_subjects & test_subjects
    assert train and val and test


def test_score_prefers_the_accurate_model():
    truth = np.linspace(10, 70, 40)
    good = truth + np.random.default_rng(0).normal(0, 2, truth.size)
    bad = np.full_like(truth, truth.mean())
    assert score(truth, good)["mae"] < score(truth, bad)["mae"]
    assert score(truth, good)["auroc_ef_lt_50"] > 0.9


def test_auroc_uses_low_ef_as_positive():
    truth = np.array([10.0, 20.0, 60.0, 70.0])
    assert score(truth, truth.copy())["auroc_ef_lt_50"] == 1.0
    assert score(truth, -truth)["auroc_ef_lt_50"] == 0.0


def test_calibration_removes_a_known_bias():
    rng = np.random.default_rng(1)
    truth = rng.uniform(20, 70, 200)
    predicted = 0.6 * truth + 15 + rng.normal(0, 1, truth.size)
    slope, intercept = fit_calibration(truth, predicted)
    assert abs(slope - 1 / 0.6) < 0.15
    calibrated = slope * predicted + intercept
    assert abs(np.mean(calibrated) - np.mean(truth)) < 2.0
    assert score(truth, calibrated)["mae"] < score(truth, predicted)["mae"]


def test_bootstrap_reports_interval_and_handles_zero_rounds():
    truth = np.linspace(20, 70, 30)
    predicted = truth + np.random.default_rng(2).normal(0, 4, truth.size)
    subjects = [f"P{i % 10:03d}" for i in range(truth.size)]
    result = bootstrap_ci(truth, predicted, subjects, rounds=50, seed=0)
    low, high = result["mae_ci95"]
    assert low < high
    assert bootstrap_ci(truth, predicted, subjects, rounds=0) == {"skipped": "bootstrap rounds = 0"}
