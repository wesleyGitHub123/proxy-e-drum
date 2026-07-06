"""Slice 1 — the Phase 0 acceptance gate, end-to-end (phase0-plan).

Scripted hits → the REAL capture pipeline → session.jsonl → recovering reader
→ fold → re-serialize → byte-compare. Plus the crash-cut sweep: the file must
recover to a valid session no matter which byte the power died on.

This exercises every contract Phase 0 exists to lock, with zero hardware.
"""

from __future__ import annotations

import pytest

from edrum.engine.records import (
    BookmarkRecord,
    CtrlRecord,
    EventRecord,
    GridEndRecord,
    GridStartRecord,
    MetaRecord,
    SessionEndRecord,
    to_line,
)
from edrum.engine.session import GridSegment, reduce_session
from edrum.io.capture import run_capture
from edrum.io.clock import FakeClock
from edrum.io.logfile import InvalidLogError, SessionWriter, read_log
from edrum.io.sources import FakeSource, FileSource, new_session_meta


def _scripted_session() -> tuple[MetaRecord, list]:
    """A realistic mixed session: warmup (grid-free) then a declared grade span."""
    clock = FakeClock(start_iso="2026-07-06T12:00:00+08:00")
    meta = new_session_meta(clock, kit_profile_id="td02k", user_id="local")
    records = [
        # warmup — click may be sounding; nothing is declared, nothing gridded
        EventRecord(t=100, note=42, velocity=60, channel=9),
        CtrlRecord(t=150, msg={"type": "control_change", "channel": 9, "control": 4, "value": 20}),
        EventRecord(t=600, note=38, velocity=75, channel=9),
        BookmarkRecord(t=800),
        # declared grade span — snapshots the running click
        GridStartRecord(t=2000, bpm=100, subdiv=4, downbeat_t=2000),
        EventRecord(t=2004, note=36, velocity=101, channel=9),
        EventRecord(t=2601, note=38, velocity=96, channel=9),
        EventRecord(t=3197, note=36, velocity=99, channel=9),
        GridEndRecord(t=4400),
        # post-span free play
        EventRecord(t=5000, note=49, velocity=110, channel=9),
        SessionEndRecord(t=5500),
    ]
    return meta, records


def test_slice1_full_roundtrip_byte_identical(tmp_path):
    """The spec Phase 0 validation, verbatim: capture a session, replay from
    file, byte-identical event stream."""
    meta, records = _scripted_session()
    path = tmp_path / "session.jsonl"

    # capture through the real pipeline
    writer = SessionWriter(path, meta)
    count = run_capture(FakeSource(meta, records), writer)
    writer.close()
    assert count == len(records)

    # replay through the real FileSource
    source = FileSource(path)
    replayed_meta = source.open()
    replayed = list(source.events())
    assert replayed_meta == meta
    assert replayed == records  # identical stream, either producer (spec §1)
    assert not source.recovery.truncated

    # byte-identity: re-serialization reproduces the file exactly
    reconstructed = to_line(replayed_meta) + b"".join(to_line(r) for r in replayed)
    assert reconstructed == path.read_bytes()

    # and the fold reads the truth out of it
    session = reduce_session(replayed_meta, replayed)
    assert session.grid_track == [GridSegment(2000, 4400, 100, 4, 2000)]
    assert len(session.events) == 6
    assert session.bookmarks == [800]
    assert session.end_t == 5500
    assert session.warnings == []


def test_slice1_crash_cut_sweep(tmp_path):
    """Power can die on any byte; recovery must always yield a valid prefix
    (truncate-to-last-complete-line, capture spec §6)."""
    meta, records = _scripted_session()
    path = tmp_path / "session.jsonl"
    writer = SessionWriter(path, meta)
    run_capture(FakeSource(meta, records), writer)
    writer.close()
    data = path.read_bytes()
    meta_line_len = data.index(b"\n") + 1

    recovered_counts = []
    for cut in range(len(data) + 1):
        crashed = tmp_path / "crashed.jsonl"
        if crashed.exists():
            crashed.unlink()
        crashed.write_bytes(data[:cut])

        if cut == 0:
            with pytest.raises(InvalidLogError):
                read_log(crashed)
            continue
        if cut < meta_line_len:
            # line 1 never completed: no meta, no session — correct refusal
            with pytest.raises(InvalidLogError):
                read_log(crashed)
            continue

        lr = read_log(crashed)
        # every complete line survived; the torn tail was dropped, file untouched
        expected_complete = data[:cut].rfind(b"\n") + 1
        reconstructed = to_line(lr.meta) + b"".join(to_line(r) for r in lr.records)
        assert reconstructed == data[:expected_complete]
        assert crashed.read_bytes() == data[:cut]
        # and the fold never fails on a recovered prefix
        session = reduce_session(lr.meta, lr.records)
        assert session.meta == meta
        recovered_counts.append(len(lr.records))

    # sanity: the sweep actually spanned partial states up to the full session
    assert min(recovered_counts) == 0
    assert max(recovered_counts) == len(records)


def test_slice1_output_matches_golden_shape(tmp_path):
    """The slice-1 artifact and the checked-in fixtures share one canonical
    form: any producer that passes one passes the other."""
    meta, records = _scripted_session()
    path = tmp_path / "session.jsonl"
    writer = SessionWriter(path, meta)
    run_capture(FakeSource(meta, records), writer)
    writer.close()
    for line in path.read_bytes().splitlines(keepends=True):
        from edrum.engine.records import parse_line

        assert to_line(parse_line(line)) == line
