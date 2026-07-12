"""Golden fixtures: the executable specification of the firmware↔brain interface.

These byte-frozen files are permanent regression tests (spec §10: the
validation gate a phase passes becomes its regression test) AND the
conformance suite any future producer — the ESP32 box included — must satisfy
byte-for-byte. If a change to the serde breaks one of these, that change is
breaking the corpus contract: stop and think, do not regenerate casually.
"""

from __future__ import annotations

import pytest

from edrum.engine.records import UnknownRecord, parse_line, to_line
from edrum.engine.session import GridSegment, EnrollmentSpan
from edrum.io.logfile import load_session, read_log
from tests.conftest import FIXTURES_DIR

BYTE_STABLE_FIXTURES = [
    "minimal.jsonl",
    "declarations.jsonl",
    "warmup_no_grid.jsonl",
    "anonymous_enroll.jsonl",
]


@pytest.mark.parametrize("name", BYTE_STABLE_FIXTURES)
def test_golden_byte_identity(name):
    """Canonical serde is FROZEN (micro-decision 6): parse → re-serialize is
    byte-identical for every checked-in canonical log."""
    data = (FIXTURES_DIR / name).read_bytes()
    lr = read_log(FIXTURES_DIR / name)
    reconstructed = to_line(lr.meta) + b"".join(to_line(r) for r in lr.records)
    assert reconstructed == data
    assert not lr.recovery.truncated


@pytest.mark.parametrize("name", BYTE_STABLE_FIXTURES)
def test_golden_line_level_roundtrip(name):
    for line in (FIXTURES_DIR / name).read_bytes().splitlines(keepends=True):
        assert to_line(parse_line(line)) == line


def test_minimal_folds():
    session, recovery = load_session(FIXTURES_DIR / "minimal.jsonl")
    assert [e.note for e in session.events] == [36, 38, 42]
    assert session.grid_track == [] and session.end_t == 1500
    assert recovery.has_session_end


def test_declarations_fold_to_expected_reduction():
    session, _ = load_session(FIXTURES_DIR / "declarations.jsonl")
    assert session.grid_track == [GridSegment(1000, 3000, 120, 4, 1000)]
    assert session.enrollment_spans == [EnrollmentSpan(4000, 6000, "basic-rock", 120, 4, 4000)]
    assert session.bookmarks == [3500]
    assert len(session.events) == 6  # events identical inside and outside spans
    assert len(session.ctrl) == 2  # CC4 + note_off: preserved-but-invisible
    assert session.warnings == []


def test_warmup_no_grid_stays_grid_empty():
    """Invariant 1 (§1A): empty grid is the truthful picture, not a missing
    feature — nothing may infer a grid onto undeclared playing."""
    session, _ = load_session(FIXTURES_DIR / "warmup_no_grid.jsonl")
    assert session.grid_track == []
    assert session.enrollment_spans == []
    assert len(session.events) == 8


def test_anonymous_enroll_fold_yields_none_profile_ref():
    """The zero-friction enrollment path (architecture review, 2026-07-11):
    no label at capture time folds to profile_ref=None, never a synthesized
    name — this fixture is the producer-side conformance counterpart to
    declarations.jsonl's labeled span."""
    session, _ = load_session(FIXTURES_DIR / "anonymous_enroll.jsonl")
    assert session.enrollment_spans == [EnrollmentSpan(1000, 3000, None, 120, 4, 1000)]
    assert session.grid_track == [GridSegment(1000, 3000, 120, 4, 1000)]
    assert session.warnings == []


def test_truncated_fixture_recovers():
    """Crash recovery = truncate to the last complete line (spec §3/§6)."""
    lr = read_log(FIXTURES_DIR / "truncated.jsonl")
    assert lr.recovery.truncated
    assert lr.recovery.dropped_bytes > 0
    assert not lr.recovery.has_session_end
    assert [r.t for r in lr.records] == [100, 600, 1100]  # valid up to the tear


def test_forward_compat_fixture():
    """Reader policy: a future-minor producer's file parses today."""
    lr = read_log(FIXTURES_DIR / "forward_compat.jsonl")
    assert lr.meta.session_id == "44444444aaaa4bbb8ccc000000000004"  # unknown meta field ignored
    unknown = [r for r in lr.records if isinstance(r, UnknownRecord)]
    assert {u.type for u in unknown} == {"pause", "resume"}  # preserved, not errors
    session_events = [r for r in lr.records if getattr(r, "note", None) is not None]
    assert len(session_events) == 2  # unknown event field ignored
    # unknown-type lines re-emit byte-faithfully
    for u in unknown:
        assert to_line(parse_line(u.raw_line)) == u.raw_line
