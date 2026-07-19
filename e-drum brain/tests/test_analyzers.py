"""S15 analyzer semantics — micro-decisions 7-10, 15, 18 in executable form.

Constructed sessions with analytically known geometry; the golden-VALUE
fixture suite (test_analyze_golden.py) freezes the numbers end-to-end
through the real file pipeline.
"""

from __future__ import annotations

from pathlib import Path

from edrum.engine.analysis import run_analyses
from edrum.engine.analyzers.ioi import PAIRS, SUMMARY
from edrum.engine.analyzers.timing import CONSISTENCY, DEVIATIONS, RUSH_DRAG
from edrum.engine.analyzers.velocity import OBSERVATIONS, PER_LANE, PER_NOTE
from edrum.engine.profiles import load_profile
from edrum.engine.session import GridSegment

from tests.test_analysis import make_session

PROFILES_DIR = Path(__file__).parent.parent / "profiles"

# tick = 60000/120/4 = 125 ms; downbeat at 1000
SEG = GridSegment(start_t=1000, end_t=3000, bpm=120, subdiv=4, downbeat_t=1000)


def by_scope(report, analyzer_id):
    return {r.scope: r for r in report.results if r.analyzer_id == analyzer_id}


def only(report, analyzer_id):
    rs = [r for r in report.results if r.analyzer_id == analyzer_id]
    assert len(rs) == 1, f"expected one {analyzer_id} result, got {len(rs)}"
    return rs[0]


# ---------------------------------------------------------------------------
# timing.deviations (Tier 1)
# ---------------------------------------------------------------------------

def test_deviation_geometry_and_sign():
    """+10 late (drag), -10 early (rush); exact hits 0 (micro-decision 7/8′)."""
    session = make_session(
        events=[(1000, 36, 100), (1135, 38, 90), (1240, 36, 95)],
        grid=[SEG],
    )
    rows = only(run_analyses(session, analyzers=[DEVIATIONS]), "timing.deviations").value
    assert [r.deviation_ms for r in rows] == [0.0, 10.0, -10.0]
    assert [r.gridline_t for r in rows] == [1000.0, 1125.0, 1250.0]
    assert [r.k for r in rows] == [0, 1, 2]


def test_calibration_applied_before_assignment():
    """micro-decision 7: t_corrected = t - offset, THEN nearest-line. A hit
    at t=1070 with offset 60 corrects to 1010 → gridline 1000, dev +10
    (uncorrected it would grade against 1125 as -55)."""
    session = make_session(events=[(1070, 36, 100)], grid=[SEG], calibration=60)
    row = only(run_analyses(session, analyzers=[DEVIATIONS]), "timing.deviations").value[0]
    assert row.t_corrected == 1010.0
    assert row.gridline_t == 1000.0 and row.deviation_ms == 10.0


def test_uncalibrated_flagged_calibrated_not():
    session = make_session(events=[(1000, 36, 100)], grid=[SEG])
    q = only(run_analyses(session, analyzers=[DEVIATIONS]), "timing.deviations").quality
    assert q.calibrated is False and "uncalibrated" in q.flags

    session2 = make_session(events=[(1000, 36, 100)], grid=[SEG], calibration=0)
    q2 = only(run_analyses(session2, analyzers=[DEVIATIONS]), "timing.deviations").quality
    assert q2.calibrated is True and "uncalibrated" not in q2.flags


def test_ambiguity_counted_and_included():
    """|dev| > 0.35·tick (43.75 ms) → ambiguous, still in the table & stats."""
    session = make_session(
        events=[(1000, 36, 100), (1050, 36, 100)],  # devs 0, +50
        grid=[SEG],
    )
    report = run_analyses(session, analyzers=[DEVIATIONS, RUSH_DRAG])
    dev = only(report, "timing.deviations")
    assert [r.ambiguous for r in dev.value] == [False, True]
    assert dev.quality.n == 2 and dev.quality.n_ambiguous == 1
    seg_mean = by_scope(report, "timing.rush_drag")[dev.scope]
    assert seg_mean.value == 25.0  # ambiguous events still included (md-10)


def test_free_play_not_graded_and_membership_inclusive():
    session = make_session(
        events=[(999, 36, 100), (1000, 36, 100), (3000, 36, 100), (3001, 36, 100)],
        grid=[SEG],
    )
    rows = only(run_analyses(session, analyzers=[DEVIATIONS]), "timing.deviations").value
    assert [r.t for r in rows] == [1000, 3000]  # inclusive [start_t, end_t]


def test_shared_boundary_later_segment_wins():
    a = GridSegment(start_t=1000, end_t=2000, bpm=120, subdiv=4, downbeat_t=1000)
    b = GridSegment(start_t=2000, end_t=3000, bpm=100, subdiv=4, downbeat_t=2000)
    session = make_session(events=[(2000, 36, 100)], grid=[a, b])
    report = run_analyses(session, analyzers=[DEVIATIONS])
    scoped = by_scope(report, "timing.deviations")
    seg_a = next(s for s in scoped if s.bpm == 120)
    seg_b = next(s for s in scoped if s.bpm == 100)
    assert scoped[seg_a].quality.n == 0  # empty segment still gets a result
    assert scoped[seg_b].quality.n == 1


def test_excluded_rows_marked_not_counted():
    """micro-decision 18: trigger-span events stay in the table (drawable),
    out of n and out of the aggregates."""
    session = make_session(
        events=[(1000, 36, 100), (1125, 36, 100), (2500, 36, 127), (2502, 49, 127)],
        grid=[SEG],
        trigger_spans=[(2500, 2502)],
    )
    report = run_analyses(session, analyzers=[DEVIATIONS, RUSH_DRAG, CONSISTENCY])
    dev = only(report, "timing.deviations")
    assert len(dev.value) == 4  # all four rows present
    assert [r.excluded for r in dev.value] == [False, False, True, True]
    assert dev.quality.n == 2 and dev.quality.n_excluded == 2
    seg_scope = dev.scope
    assert by_scope(report, "timing.rush_drag")[seg_scope].value == 0.0  # gesture hits out
    # per-note grouping never sees excluded rows
    note_scopes = [s for s in by_scope(report, "timing.rush_drag") if s.note is not None]
    assert {s.note for s in note_scopes} == {36}


# ---------------------------------------------------------------------------
# rush_drag / consistency (Tier 2)
# ---------------------------------------------------------------------------

def test_consistency_none_under_n2_and_pooled_bpm():
    a = GridSegment(start_t=1000, end_t=2000, bpm=120, subdiv=4, downbeat_t=1000)
    b = GridSegment(start_t=4000, end_t=5000, bpm=120, subdiv=4, downbeat_t=4000)
    session = make_session(
        events=[(1010, 36, 100), (4000, 36, 100), (4135, 38, 90)],  # devs +10 | 0, +10
        grid=[a, b],
    )
    report = run_analyses(session, analyzers=[DEVIATIONS, RUSH_DRAG, CONSISTENCY])
    cons = by_scope(report, "timing.consistency")
    seg_a_scope = next(s for s in cons if s.span is not None and s.span.start_t == 1000)
    assert cons[seg_a_scope].value is None and cons[seg_a_scope].quality.n == 1
    pooled_scope = next(s for s in cons if s.span is None)
    assert pooled_scope.bpm == 120 and pooled_scope.subdiv == 4
    assert cons[pooled_scope].quality.n == 3  # both segments pooled
    rush = by_scope(report, "timing.rush_drag")
    assert abs(rush[pooled_scope].value - 20.0 / 3.0) < 1e-9


def test_lane_views_repoint_with_profile():
    """The SSD property at the metrics level: lane-scoped results are a
    profile-dependent view; note-scoped results never change (§4.2)."""
    session = make_session(events=[(1000, 36, 100), (1125, 38, 90)], grid=[SEG])
    profile = load_profile(PROFILES_DIR / "td02k.json")
    with_profile = run_analyses(session, profile=profile, analyzers=[DEVIATIONS, RUSH_DRAG])
    without = run_analyses(session, analyzers=[DEVIATIONS, RUSH_DRAG])
    lanes = [s.lane for s in by_scope(with_profile, "timing.rush_drag") if s.lane]
    assert sorted(lanes) == ["KICK", "SNARE"]
    assert not [s for s in by_scope(without, "timing.rush_drag") if s.lane]
    # note-scoped values identical either way
    note36 = next(
        r for s, r in by_scope(with_profile, "timing.rush_drag").items() if s.note == 36
    )
    note36_np = next(
        r for s, r in by_scope(without, "timing.rush_drag").items() if s.note == 36
    )
    assert note36.value == note36_np.value


# ---------------------------------------------------------------------------
# velocity
# ---------------------------------------------------------------------------

def test_velocity_stats_and_exclusion():
    session = make_session(
        events=[(100, 38, 80), (200, 38, 90), (300, 38, 127)],
        trigger_spans=[(300, 300)],
    )
    report = run_analyses(session, analyzers=[OBSERVATIONS, PER_NOTE])
    obs = only(report, "velocity.observations")
    assert obs.quality.n == 2 and obs.quality.n_excluded == 1
    stats = only(report, "velocity.per_note")
    assert stats.scope.note == 38
    assert stats.value.mean == 85.0  # 127 excluded
    assert abs(stats.value.std - 7.0710678118654755) < 1e-12


def test_velocity_per_lane_needs_profile():
    session = make_session(events=[(100, 38, 80)])
    report = run_analyses(session, analyzers=[OBSERVATIONS, PER_LANE])
    assert [f.analyzer_id for f in report.refusals] == ["velocity.per_lane"]
    profile = load_profile(PROFILES_DIR / "td02k.json")
    report2 = run_analyses(session, profile=profile, analyzers=[OBSERVATIONS, PER_LANE])
    lane = only(report2, "velocity.per_lane")
    assert lane.scope.lane == "SNARE" and lane.scope.articulation == "head"


# ---------------------------------------------------------------------------
# ioi
# ---------------------------------------------------------------------------

def test_ioi_pairs_pooled_per_note_and_bridging():
    """Bridging rule: a pair spanning an excluded same-stream member is
    dropped and counted; other-stream exclusions don't break a pair."""
    session = make_session(
        events=[
            (1000, 36, 100),
            (1200, 38, 90),
            (1500, 36, 100),
            (1700, 36, 127),  # excluded (gesture)
            (1702, 49, 127),  # excluded (gesture)
            (2000, 36, 100),
            (2400, 38, 90),
        ],
        trigger_spans=[(1700, 1702)],
    )
    report = run_analyses(session, analyzers=[PAIRS, SUMMARY])
    scoped = by_scope(report, "ioi.pairs")

    pooled = next(r for s, r in scoped.items() if s.note is None)
    assert [row.ioi_ms for row in pooled.value] == [200, 300, 400]  # 1500→2000 dropped
    assert pooled.quality.n == 3 and pooled.quality.n_excluded == 1

    kick = next(r for s, r in scoped.items() if s.note == 36)
    assert [row.ioi_ms for row in kick.value] == [500]  # 1500→2000 bridges excluded kick
    assert kick.quality.n_excluded == 1

    snare = next(r for s, r in scoped.items() if s.note == 38)
    # the excluded events are not snare-stream members: pair survives
    assert [row.ioi_ms for row in snare.value] == [1200]
    assert snare.quality.n_excluded == 0

    summary = by_scope(report, "ioi.summary")
    pooled_sum = next(r for s, r in summary.items() if s.note is None)
    assert pooled_sum.value.mean == 300.0 and pooled_sum.value.std == 100.0
