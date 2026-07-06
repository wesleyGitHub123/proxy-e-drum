"""S7 tests: the receiver loop."""

from __future__ import annotations

from edrum.engine.records import BookmarkRecord, EventRecord, SessionEndRecord
from edrum.io.capture import run_capture
from edrum.io.logfile import SessionWriter, read_log
from edrum.io.sources import FakeSource
from tests.conftest import make_meta


def test_run_capture_writes_stream_in_order(tmp_path):
    meta = make_meta()
    records = [
        EventRecord(t=10, note=36, velocity=90, channel=9),
        BookmarkRecord(t=15),
        EventRecord(t=20, note=38, velocity=80, channel=9),
    ]
    path = tmp_path / "s.jsonl"
    writer = SessionWriter(path, meta)
    count = run_capture(FakeSource(meta, records), writer)
    writer.close(end_t=25)
    assert count == 3
    lr = read_log(path)
    assert lr.records[:-1] == records
    assert lr.records[-1] == SessionEndRecord(t=25)


def test_run_capture_with_scripted_session_end(tmp_path):
    """A source may script its own session_end; close() must not duplicate it."""
    meta = make_meta()
    records = [EventRecord(t=10, note=36, velocity=90), SessionEndRecord(t=99)]
    path = tmp_path / "s.jsonl"
    writer = SessionWriter(path, meta)
    run_capture(FakeSource(meta, records), writer)
    writer.close()
    data = path.read_bytes()
    assert data.count(b'"session_end"') == 1
    assert read_log(path).records[-1] == SessionEndRecord(t=99)


def test_run_capture_on_record_hook(tmp_path):
    meta = make_meta()
    seen = []
    writer = SessionWriter(tmp_path / "s.jsonl", meta)
    run_capture(
        FakeSource(meta, [EventRecord(t=5, note=42, velocity=60)]),
        writer,
        on_record=seen.append,
    )
    writer.close()
    assert seen == [EventRecord(t=5, note=42, velocity=60)]


def test_unclean_stop_leaves_recoverable_file(tmp_path):
    """Crash mid-capture: no session_end, file still valid (capture spec §6)."""
    meta = make_meta()
    writer = SessionWriter(tmp_path / "s.jsonl", meta)
    run_capture(FakeSource(meta, [EventRecord(t=10, note=36, velocity=90)]), writer)
    writer.abort()  # simulated crash: no close, no session_end
    lr = read_log(tmp_path / "s.jsonl")
    assert not lr.recovery.has_session_end
    assert lr.records == [EventRecord(t=10, note=36, velocity=90)]
