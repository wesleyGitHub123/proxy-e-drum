"""S2 tests: append-only writer, recovering reader, crash simulation."""

from __future__ import annotations

import json

import pytest

from edrum.engine.records import (
    BookmarkRecord,
    EventRecord,
    SessionEndRecord,
    to_line,
)
from edrum.io.logfile import (
    InvalidLogError,
    LogCorruptionError,
    LogError,
    SessionWriter,
    read_log,
    session_filename,
)
from tests.conftest import make_meta


def test_session_filename_convention():
    name = session_filename("2026-07-06T14:30:05+08:00", "a1b2c3d4e5f60718293a4b5c6d7e8f90")
    assert name == "20260706T143005_a1b2c3d4.jsonl"


def test_writer_writes_meta_first_and_appends(tmp_path, meta):
    path = tmp_path / "s.jsonl"
    w = SessionWriter(path, meta)
    w.append(EventRecord(t=10, note=36, velocity=90, channel=9))
    w.close(end_t=20)
    lines = path.read_bytes().split(b"\n")
    assert json.loads(lines[0])["type"] == "meta"
    assert json.loads(lines[1])["type"] == "event"
    assert json.loads(lines[2]) == {"type": "session_end", "t": 20}


def test_writer_refuses_existing_file(tmp_path, meta):
    path = tmp_path / "s.jsonl"
    path.write_bytes(b"existing")
    with pytest.raises(FileExistsError):
        SessionWriter(path, meta)  # files are never rewritten (spec §3)


def test_writer_is_strictly_appending(tmp_path, meta):
    """Observable append-only property: every write strictly extends the file."""
    path = tmp_path / "s.jsonl"
    w = SessionWriter(path, meta)
    previous = path.read_bytes()
    for t in (5, 10, 15):
        w.append(EventRecord(t=t, note=38, velocity=70))
        now = path.read_bytes()
        assert now.startswith(previous) and len(now) > len(previous)
        previous = now
    w.close()


def test_writer_rejects_non_monotonic_t(tmp_path, meta):
    w = SessionWriter(tmp_path / "s.jsonl", meta)
    w.append(EventRecord(t=100, note=36, velocity=80))
    with pytest.raises(LogError):
        w.append(EventRecord(t=99, note=36, velocity=80))
    w.abort()


def test_writer_rejects_second_meta_and_post_end_appends(tmp_path, meta):
    w = SessionWriter(tmp_path / "s.jsonl", meta)
    with pytest.raises(LogError):
        w.append(make_meta())
    w.append(SessionEndRecord(t=50))  # scripted session_end (FakeSource path)
    with pytest.raises(LogError):
        w.append(BookmarkRecord(t=60))
    w.close()  # must not write a second session_end
    data = (tmp_path / "s.jsonl").read_bytes()
    assert data.count(b'"session_end"') == 1


def test_close_defaults_end_t_to_last_t(tmp_path, meta):
    w = SessionWriter(tmp_path / "s.jsonl", meta)
    w.append(EventRecord(t=123, note=36, velocity=80))
    w.close()
    lr = read_log(tmp_path / "s.jsonl")
    assert lr.records[-1] == SessionEndRecord(t=123)


def test_abort_leaves_valid_recoverable_log(tmp_path, meta):
    path = tmp_path / "s.jsonl"
    w = SessionWriter(path, meta)
    w.append(EventRecord(t=10, note=36, velocity=90))
    w.abort()  # unclean stop: no session_end
    lr = read_log(path)
    assert not lr.recovery.has_session_end
    assert not lr.recovery.truncated
    assert lr.records == [EventRecord(t=10, note=36, velocity=90)]


# ---------------------------------------------------------------------------
# crash recovery: truncate to the last complete line
# ---------------------------------------------------------------------------

def _write_sample(tmp_path, meta):
    path = tmp_path / "s.jsonl"
    w = SessionWriter(path, meta)
    for t, note in ((10, 36), (20, 38), (30, 42)):
        w.append(EventRecord(t=t, note=note, velocity=100, channel=9))
    w.close(end_t=40)
    return path


def test_reader_recovers_torn_tail_without_lf(tmp_path, meta):
    path = _write_sample(tmp_path, meta)
    data = path.read_bytes()
    cut = data[: len(data) - 15]  # slice into the last line, losing its LF
    crashed = tmp_path / "crashed.jsonl"
    crashed.write_bytes(cut)
    lr = read_log(crashed)
    assert lr.recovery.truncated
    assert lr.recovery.dropped_bytes > 0
    assert not lr.recovery.has_session_end
    assert [r.t for r in lr.records] == [10, 20, 30]
    # the file itself was not modified (reader never repairs on disk)
    assert crashed.read_bytes() == cut


def test_reader_drops_invalid_final_line_with_lf(tmp_path, meta):
    path = _write_sample(tmp_path, meta)
    crashed = tmp_path / "crashed.jsonl"
    crashed.write_bytes(path.read_bytes() + b'{"type":"event","t":50,"no\n')
    lr = read_log(crashed)
    assert lr.recovery.truncated
    assert lr.recovery.has_session_end  # everything before the torn line survived


def test_reader_raises_on_mid_file_corruption(tmp_path, meta):
    path = _write_sample(tmp_path, meta)
    lines = path.read_bytes().split(b"\n")
    lines[1] = b"garbage"
    corrupt = tmp_path / "corrupt.jsonl"
    corrupt.write_bytes(b"\n".join(lines))
    with pytest.raises(LogCorruptionError):
        read_log(corrupt)


def test_reader_rejects_empty_and_metaless_files(tmp_path):
    empty = tmp_path / "empty.jsonl"
    empty.write_bytes(b"")
    with pytest.raises(InvalidLogError):
        read_log(empty)
    no_meta = tmp_path / "nometa.jsonl"
    no_meta.write_bytes(to_line(EventRecord(t=1, note=36, velocity=80)))
    with pytest.raises(InvalidLogError):
        read_log(no_meta)


def test_reader_rejects_duplicate_meta(tmp_path, meta):
    path = tmp_path / "dup.jsonl"
    path.write_bytes(to_line(meta) + to_line(meta))
    with pytest.raises(LogCorruptionError):
        read_log(path)


def test_roundtrip_bytes_writer_to_reader(tmp_path, meta):
    """Phase 0 gate: written bytes == re-serialized parse of them."""
    path = _write_sample(tmp_path, meta)
    lr = read_log(path)
    reconstructed = to_line(lr.meta) + b"".join(to_line(r) for r in lr.records)
    assert reconstructed == path.read_bytes()
