"""Golden-VALUE suite (phase1-stats-plan D.6.2, spec §10).

The synthetic graded fixtures have analytically designed geometry:
    tick = 125 ms (bpm 120, subdiv 4), downbeat 1000, segment [1000, 5000]
    graded deviations by design: 0, +10, -10, +20, -20, +30, +50(ambiguous)
    a kick+crash x2 gesture burst (4 hits, vel 127) declared via
    grid_end.trigger_span — marked, drawn, and excluded from every aggregate
    free-play warmup (2 hi-hat hits) before the span — never graded

Every literal below is hand-derived from that design. These values are
WELDED: a change here is a change to shipped numbers and requires an
ANALYSIS_VERSION bump (micro-decision 11). The calibrated variant is the
identical performance shifted +15 ms through a pipeline with
calibration_offset_ms=15 — all metrics must be identical, minus the
uncalibrated flag.
"""

from __future__ import annotations

import json
import math
import subprocess
import sys
from pathlib import Path

import pytest

from edrum.engine.analysis import report_to_dict, run_analyses
from edrum.engine.profiles import load_profile
from edrum.engine.session import reduce_session
from edrum.io.logfile import read_log

from tests.conftest import FIXTURES_DIR

PROFILES_DIR = Path(__file__).parent.parent / "profiles"
TOL = 1e-6


def analyze_fixture(name: str):
    lr = read_log(FIXTURES_DIR / name)
    session = reduce_session(lr.meta, lr.records)
    profile = load_profile(PROFILES_DIR / "td02k.json")
    return run_analyses(session, profile=profile)


def scoped(report, analyzer_id):
    return {r.scope: r for r in report.results if r.analyzer_id == analyzer_id}


def seg_scope(scopes):
    return next(s for s in scopes if s.span is not None and s.lane is None and s.note is None)


def note_scope(scopes, note, in_span=True):
    return next(
        s for s in scopes if s.note == note and ((s.span is not None) == in_span)
    )


@pytest.fixture(scope="module")
def report():
    return analyze_fixture("graded_synthetic.jsonl")


def test_deviation_table_geometry(report):
    dev = scoped(report, "timing.deviations")
    rows = dev[seg_scope(dev)].value
    included = [r for r in rows if not r.excluded]
    assert [r.deviation_ms for r in included] == [0.0, 10.0, -10.0, 20.0, -20.0, 30.0, 50.0]
    assert [r.ambiguous for r in included] == [False] * 6 + [True]
    assert [r.t for r in rows if r.excluded] == [4600, 4602, 4900, 4902]
    q = dev[seg_scope(dev)].quality
    assert q.n == 7 and q.n_excluded == 4 and q.n_ambiguous == 1
    assert q.calibrated is False and "uncalibrated" in q.flags


def test_rush_drag_golden(report):
    rush = scoped(report, "timing.rush_drag")
    assert math.isclose(rush[seg_scope(rush)].value, 11.428571428571429, abs_tol=TOL)
    assert math.isclose(rush[note_scope(rush, 36)].value, -10.0, abs_tol=TOL)
    assert math.isclose(rush[note_scope(rush, 38)].value, 20.0, abs_tol=TOL)
    assert math.isclose(rush[note_scope(rush, 42)].value, 50.0, abs_tol=TOL)
    pooled = next(s for s in rush if s.span is None)
    assert (pooled.bpm, pooled.subdiv) == (120, 4)
    assert math.isclose(rush[pooled].value, 11.428571428571429, abs_tol=TOL)
    # gesture notes never surface as timing groups (all their rows excluded)
    assert not any(s.note == 49 for s in rush)


def test_consistency_golden(report):
    cons = scoped(report, "timing.consistency")
    assert math.isclose(cons[seg_scope(cons)].value, 24.10295378065479, abs_tol=TOL)
    assert math.isclose(cons[note_scope(cons, 36)].value, 10.0, abs_tol=TOL)
    assert math.isclose(cons[note_scope(cons, 38)].value, 10.0, abs_tol=TOL)
    assert cons[note_scope(cons, 42)].value is None  # n=1 → None, never 0.0


def test_velocity_golden(report):
    per_note = scoped(report, "velocity.per_note")
    v36 = per_note[note_scope(per_note, 36, in_span=False)].value
    assert math.isclose(v36.mean, 100.0, abs_tol=TOL) and v36.std == 0.0
    v42 = per_note[note_scope(per_note, 42, in_span=False)].value
    assert math.isclose(v42.mean, 63.333333333333336, abs_tol=TOL)
    assert math.isclose(v42.std, 5.773502691896257, abs_tol=TOL)
    # note 49 exists only as excluded gesture hits → no stats group at all
    assert not any(s.note == 49 for s in per_note)
    lanes = scoped(report, "velocity.per_lane")
    kick = next(r for s, r in lanes.items() if s.lane == "KICK")
    assert math.isclose(kick.value.mean, 100.0, abs_tol=TOL) and kick.quality.n == 3


def test_ioi_golden(report):
    pairs = scoped(report, "ioi.pairs")
    pooled = next(r for s, r in pairs.items() if s.note is None)
    assert [row.ioi_ms for row in pooled.value] == [250, 550, 135, 105, 280, 210, 175, 145]
    summary = scoped(report, "ioi.summary")
    pooled_sum = next(r for s, r in summary.items() if s.note is None)
    assert math.isclose(pooled_sum.value.mean, 231.25, abs_tol=TOL)
    assert math.isclose(pooled_sum.value.std, 141.71777789476016, abs_tol=TOL)
    kick_sum = next(r for s, r in summary.items() if s.note == 36)
    assert math.isclose(kick_sum.value.mean, 365.0, abs_tol=TOL)
    assert math.isclose(kick_sum.value.std, 176.7766952966369, abs_tol=TOL)
    hh_sum = next(r for s, r in summary.items() if s.note == 42)
    assert math.isclose(hh_sum.value.mean, 925.0, abs_tol=TOL)
    assert math.isclose(hh_sum.value.std, 954.5941546018391, abs_tol=TOL)
    crash = next(r for s, r in pairs.items() if s.note == 49)
    assert crash.quality.n == 0  # both crash hits excluded → no pairs


def test_calibrated_variant_identical_numbers():
    """+15 ms pipeline shift + calibration_offset_ms=15 → identical metrics,
    calibrated=True, no uncalibrated flag (micro-decision 7)."""
    cal = analyze_fixture("graded_synthetic_calibrated.jsonl")
    assert cal.calibrated is True
    rush = scoped(cal, "timing.rush_drag")
    r = rush[seg_scope(rush)]
    assert math.isclose(r.value, 11.428571428571429, abs_tol=TOL)
    assert r.quality.calibrated is True and "uncalibrated" not in r.quality.flags
    cons = scoped(cal, "timing.consistency")
    assert math.isclose(cons[seg_scope(cons)].value, 24.10295378065479, abs_tol=TOL)
    dev = scoped(cal, "timing.deviations")
    included = [x for x in dev[seg_scope(dev)].value if not x.excluded]
    assert [x.deviation_ms for x in included] == [0.0, 10.0, -10.0, 20.0, -20.0, 30.0, 50.0]


def test_fixture_replays_byte_identical():
    """The new fixtures pass the Phase 0 gate — trigger_span round-trips."""
    for name in ("graded_synthetic.jsonl", "graded_synthetic_calibrated.jsonl"):
        path = FIXTURES_DIR / name
        proc = subprocess.run(
            [sys.executable, "-m", "edrum.cli.main", "replay", str(path)],
            capture_output=True, text=True,
        )
        assert proc.returncode == 0, proc.stdout + proc.stderr
        assert "PASS byte-identical" in proc.stdout


def test_report_json_stable_shape(report):
    """Not a byte-freeze (micro-decision 11) — just: valid JSON, refusal-free
    on the graded fixture, provenance present."""
    d = json.loads(json.dumps(report_to_dict(report)))
    assert d["refusals"] == []
    assert d["analysis_version"] == 1
    assert d["profile_id"] == "td02k"
    assert any(r["analyzer_id"] == "timing.rush_drag" for r in d["results"])
