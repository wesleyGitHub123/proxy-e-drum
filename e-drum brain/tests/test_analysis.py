"""S14 analysis core — machinery tests with scripted fake analyzers.

These prove the runner independent of any real metric: dependency closure &
ordering, refusal gating/propagation, the quality meet (micro-decision 17),
substrate construction (exclusion marks, lane views), and report_to_dict.
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from edrum.engine.analysis import (
    AnalysisInputs,
    Analyzer,
    MetricResult,
    Quality,
    Requirements,
    Scope,
    build_inputs,
    report_to_dict,
    run_analyses,
)
from edrum.engine.profiles import load_profile
from edrum.engine.records import EventRecord
from edrum.engine.session import GridSegment, Session

from tests.conftest import make_meta

PROFILES_DIR = Path(__file__).parent.parent / "profiles"


def make_session(
    events=(),
    grid=(),
    trigger_spans=(),
    calibration=None,
    start_iso="2026-07-06T12:00:00+08:00",
) -> Session:
    s = Session(meta=make_meta(calibration_offset_ms=calibration, start_iso=start_iso))
    s.events = [EventRecord(t=t, note=note, velocity=vel, channel=9) for t, note, vel in events]
    s.grid_track = list(grid)
    s.trigger_spans = list(trigger_spans)
    return s


def result_for(analyzer: Analyzer, value=1.0, scope=Scope(), quality=None) -> MetricResult:
    return MetricResult(
        analyzer_id=analyzer.id,
        analyzer_version=analyzer.version,
        scope=scope,
        unit="x",
        value=value,
        quality=quality or Quality(n=1),
    )


def fake(analyzer_id, requires=Requirements(), quality=None, calls=None):
    """A fake analyzer emitting one result; records call order in `calls`."""
    holder = {}

    def _compute(inputs, upstream):
        if calls is not None:
            calls.append(analyzer_id)
        return [result_for(holder["a"], quality=quality)]

    holder["a"] = Analyzer(id=analyzer_id, version=1, requires=requires, compute=_compute)
    return holder["a"]


# ---------------------------------------------------------------------------
# substrate
# ---------------------------------------------------------------------------

def test_build_inputs_marks_exclusions_and_lane_views():
    session = make_session(
        events=[(100, 36, 90), (200, 49, 80), (300, 38, 70)],
        trigger_spans=[(150, 250)],
    )
    profile = load_profile(PROFILES_DIR / "td02k.json")
    inputs = build_inputs(session, profile)
    assert [ee.excluded for ee in inputs.events] == [False, True, False]
    assert [ee.lane for ee in inputs.events] == ["KICK", "CRASH", "SNARE"]
    assert inputs.events[2].articulation == "head"
    # exclusion span endpoints are inclusive
    session2 = make_session(events=[(150, 36, 90)], trigger_spans=[(150, 250)])
    assert build_inputs(session2).events[0].excluded is True


def test_build_inputs_without_profile_has_no_lanes():
    inputs = build_inputs(make_session(events=[(1, 36, 50)]))
    assert inputs.events[0].lane is None
    assert inputs.events[0].articulation is None
    assert inputs.calibrated is False
    assert build_inputs(make_session(calibration=12)).calibrated is True


# ---------------------------------------------------------------------------
# ordering, closure, refusals
# ---------------------------------------------------------------------------

def test_upstream_ordering_and_map():
    calls = []
    base = fake("t.base", calls=calls)
    derived_compute_saw = {}

    def _derived(inputs, upstream):
        calls.append("t.derived")
        derived_compute_saw["upstream"] = upstream
        return []

    derived = Analyzer(
        id="t.derived", version=1,
        requires=Requirements(upstream=("t.base",)), compute=_derived,
    )
    report = run_analyses(make_session(), analyzers=[derived, base])
    assert calls == ["t.base", "t.derived"]  # topo order despite request order
    assert set(derived_compute_saw["upstream"]) == {"t.base"}
    assert len(report.results) == 1


def test_requirement_gates_produce_refusals_not_errors():
    needs_grid = fake("t.grid", requires=Requirements(needs_grid=True))
    needs_profile = fake("t.prof", requires=Requirements(needs_profile=True))
    chained = fake("t.chain", requires=Requirements(upstream=("t.grid",)))
    report = run_analyses(make_session(), analyzers=[needs_grid, needs_profile, chained])
    reasons = {f.analyzer_id: f.reason for f in report.refusals}
    assert reasons == {
        "t.grid": "no_grid_segments",
        "t.prof": "no_profile",
        "t.chain": "upstream_refused",
    }
    assert report.results == ()


def test_grid_gate_passes_with_segment():
    seg = GridSegment(start_t=0, end_t=1000, bpm=120, subdiv=4, downbeat_t=0)
    needs_grid = fake("t.grid", requires=Requirements(needs_grid=True))
    report = run_analyses(make_session(grid=[seg]), analyzers=[needs_grid])
    assert report.refusals == ()
    assert len(report.results) == 1


def test_unknown_upstream_and_cycle_raise():
    bad = fake("t.bad", requires=Requirements(upstream=("t.missing",)))
    with pytest.raises(ValueError, match="unknown upstream"):
        run_analyses(make_session(), analyzers=[bad])

    a = fake("t.a", requires=Requirements(upstream=("t.b",)))
    b = fake("t.b", requires=Requirements(upstream=("t.a",)))
    with pytest.raises(ValueError, match="cycle"):
        run_analyses(make_session(), analyzers=[a, b])


def test_misstamped_result_asserts():
    def _lying(inputs, upstream):
        return [
            MetricResult(
                analyzer_id="somebody.else", analyzer_version=9,
                scope=Scope(), unit="x", value=0, quality=Quality(n=0),
            )
        ]

    liar = Analyzer(id="t.liar", version=1, requires=Requirements(), compute=_lying)
    with pytest.raises(AssertionError):
        run_analyses(make_session(), analyzers=[liar])


# ---------------------------------------------------------------------------
# quality propagation (micro-decision 17)
# ---------------------------------------------------------------------------

def test_quality_meet_inherits_uncertainty():
    base = fake(
        "t.base",
        quality=Quality(n=5, calibrated=False, confidence=0.7, flags=("uncalibrated",)),
    )
    derived = fake(
        "t.derived",
        requires=Requirements(upstream=("t.base",)),
        quality=Quality(n=5, confidence=0.9),  # own confidence higher than input's
    )
    report = run_analyses(make_session(), analyzers=[base, derived])
    by_id = {r.analyzer_id: r for r in report.results}
    q = by_id["t.derived"].quality
    assert q.calibrated is False  # AND with upstream
    assert q.confidence == 0.7  # min with upstream
    assert "uncalibrated" in q.flags  # union
    assert q.n == 5  # own sample size untouched


def test_deterministic_chain_keeps_confidence_none():
    base = fake("t.base", quality=Quality(n=3))
    derived = fake("t.derived", requires=Requirements(upstream=("t.base",)))
    report = run_analyses(make_session(), analyzers=[base, derived])
    for r in report.results:
        assert r.quality.confidence is None
        assert r.quality.calibrated is None


# ---------------------------------------------------------------------------
# report header + dict conversion
# ---------------------------------------------------------------------------

def test_undated_session_is_flagged():
    report = run_analyses(
        make_session(start_iso="1970-01-01T09:05:14+08:00"), analyzers=[]
    )
    assert any("undated" in w for w in report.warnings)
    report2 = run_analyses(make_session(), analyzers=[])
    assert not any("undated" in w for w in report2.warnings)


def test_report_header_provenance():
    profile = load_profile(PROFILES_DIR / "td02k.json")
    session = make_session(calibration=15)
    report = run_analyses(session, profile=profile, analyzers=[])
    assert report.session_id == session.meta.session_id
    assert report.analysis_version == 1
    assert report.profile_id == "td02k"
    assert report.profile_version == profile.profile_version
    assert report.calibrated is True and report.calibration_offset_ms == 15


def test_report_to_dict_is_json_serializable():
    base = fake("t.base")
    report = run_analyses(make_session(), analyzers=[base])
    encoded = json.dumps(report_to_dict(report))
    decoded = json.loads(encoded)
    assert decoded["results"][0]["analyzer_id"] == "t.base"
    assert decoded["analysis_version"] == 1


def test_default_registry_runs_everything():
    """analyzers=None → the full registry. With one event, no grid, no
    profile: every analyzer either produces results or refuses honestly."""
    report = run_analyses(make_session(events=[(100, 38, 80)]))
    produced = {r.analyzer_id for r in report.results}
    refused = {f.analyzer_id for f in report.refusals}
    from edrum.engine.analyzers import ALL

    assert produced | refused == {a.id for a in ALL}
    assert refused == {
        "timing.deviations",  # no grid segments
        "timing.rush_drag",  # upstream refused
        "timing.consistency",
        "velocity.per_lane",  # no profile
    }
