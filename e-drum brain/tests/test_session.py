"""S3 tests: the fold from record stream to Session (parsed reduction)."""

from __future__ import annotations

from edrum.engine.records import (
    BookmarkRecord,
    CtrlRecord,
    EnrollEndRecord,
    EnrollStartRecord,
    EventRecord,
    GridEndRecord,
    GridStartRecord,
    SessionEndRecord,
    UnknownRecord,
)
from edrum.engine.session import EnrollmentSpan, GridSegment, reduce_session
from tests.conftest import make_meta


def ev(t, note=38, vel=100):
    return EventRecord(t=t, note=note, velocity=vel, channel=9)


def test_empty_stream_yields_empty_session():
    s = reduce_session(make_meta(), [])
    assert s.events == [] and s.grid_track == [] and s.enrollment_spans == []
    assert s.bookmarks == [] and s.end_t is None and s.warnings == []


def test_warmup_with_click_no_declaration_yields_empty_grid():
    """Invariant 1 (§1A): sounding the click writes nothing; no declaration = no grid."""
    records = [ev(100), ev(600), ev(1100), SessionEndRecord(t=1200)]
    s = reduce_session(make_meta(), records)
    assert s.grid_track == []  # empty grid is the truthful picture, not a missing feature
    assert len(s.events) == 3
    assert s.end_t == 1200


def test_grid_span_folds_to_segment():
    records = [
        ev(500),
        GridStartRecord(t=1000, bpm=120, subdiv=4, downbeat_t=1000),
        ev(1500),
        ev(2000),
        GridEndRecord(t=3000),
        ev(3500),
        SessionEndRecord(t=4000),
    ]
    s = reduce_session(make_meta(), records)
    assert s.grid_track == [GridSegment(1000, 3000, 120, 4, 1000)]
    assert len(s.events) == 4  # events identical whether or not a span was graded


def test_param_change_closes_and_reopens_segment():
    """Spec §5: a click-param change closes the current segment at the change point."""
    records = [
        GridStartRecord(t=1000, bpm=100, subdiv=4, downbeat_t=1000),
        ev(1500),
        GridStartRecord(t=2000, bpm=120, subdiv=4, downbeat_t=2000),
        ev(2500),
        GridEndRecord(t=3000),
    ]
    s = reduce_session(make_meta(), records)
    assert s.grid_track == [
        GridSegment(1000, 2000, 100, 4, 1000),
        GridSegment(2000, 3000, 120, 4, 2000),
    ]


def test_dangling_grid_auto_closes_at_last_event_in_span():
    """Spec §5: a forgotten 'end grade' never produces an unbounded segment."""
    records = [
        ev(500),
        GridStartRecord(t=1000, bpm=120, subdiv=4, downbeat_t=1000),
        ev(1500),
        ev(2200),
        SessionEndRecord(t=9000),
    ]
    s = reduce_session(make_meta(), records)
    assert s.grid_track == [GridSegment(1000, 2200, 120, 4, 1000)]
    assert any("auto-closed" in w for w in s.warnings)


def test_dangling_grid_with_no_events_degenerates_to_start():
    records = [GridStartRecord(t=1000, bpm=120, subdiv=4, downbeat_t=1000)]
    s = reduce_session(make_meta(), records)
    assert s.grid_track == [GridSegment(1000, 1000, 120, 4, 1000)]


def test_unmatched_grid_end_warns_and_is_ignored():
    s = reduce_session(make_meta(), [GridEndRecord(t=100)])
    assert s.grid_track == []
    assert any("unmatched grid_end" in w for w in s.warnings)


def test_enrollment_span_folds_with_click_snapshot():
    records = [
        EnrollStartRecord(t=1000, profile_ref="basic-rock", bpm=120, subdiv=4, downbeat_t=1000),
        ev(1100),
        ev(1600),
        EnrollEndRecord(t=3000),
    ]
    s = reduce_session(make_meta(), records)
    assert s.enrollment_spans == [EnrollmentSpan(1000, 3000, "basic-rock", 120, 4, 1000)]


def test_anonymous_enrollment_span_folds_with_none_profile_ref():
    """No label supplied at capture time folds to profile_ref=None, not a
    synthesized name — display naming is a presentation-layer concern."""
    records = [
        EnrollStartRecord(t=1000, profile_ref=None, bpm=120, subdiv=4, downbeat_t=1000),
        ev(1100),
        ev(1600),
        EnrollEndRecord(t=3000),
    ]
    s = reduce_session(make_meta(), records)
    assert s.enrollment_spans == [EnrollmentSpan(1000, 3000, None, 120, 4, 1000)]


def test_dangling_enrollment_auto_closes():
    records = [
        EnrollStartRecord(t=1000, profile_ref="fill-a", bpm=90, subdiv=3, downbeat_t=1000),
        ev(1200),
    ]
    s = reduce_session(make_meta(), records)
    assert s.enrollment_spans == [EnrollmentSpan(1000, 1200, "fill-a", 90, 3, 1000)]


def test_bookmarks_and_ctrl_collected():
    records = [
        CtrlRecord(t=50, msg={"type": "control_change", "channel": 9, "control": 4, "value": 30}),
        BookmarkRecord(t=700),
        ev(800),
    ]
    s = reduce_session(make_meta(), records)
    assert s.bookmarks == [700]
    assert len(s.ctrl) == 1  # preserved-but-invisible: not on the events track
    assert len(s.events) == 1


def test_unknown_records_ignored_with_tally():
    unknown = UnknownRecord(type="pause", t=500, raw={"type": "pause", "t": 500}, raw_line=b'{"type":"pause","t":500}\n')
    s = reduce_session(make_meta(), [ev(100), unknown, ev(900)])
    assert len(s.events) == 2
    assert any("unknown-type" in w for w in s.warnings)


def test_non_monotonic_t_warns_never_fails():
    s = reduce_session(make_meta(), [ev(1000), ev(900)])
    assert len(s.events) == 2  # foreign files still fold
    assert any("non-monotonic" in w for w in s.warnings)


def test_grading_is_not_stored_anywhere():
    """Invariant 2 (§1A): no grade/score/offset field exists on the Session."""
    s = reduce_session(make_meta(), [ev(100)])
    forbidden = ("grade", "score", "offset", "rush", "drag", "deviation")
    for name in vars(s):
        assert not any(word in name.lower() for word in forbidden)
