"""S5 tests: stamping/classification, source equivalence, live-source queue.

python-rtmidi virtual ports are unavailable on Windows, so LiveMidiSource is
driven through ``_on_message`` directly with constructed ``mido.Message``
objects and a FakeClock — no ports needed (phase0-plan S5 testing note).
"""

from __future__ import annotations

import threading
import time

import mido
import pytest

from edrum.engine.records import CtrlRecord, EventRecord, SCHEMA_VERSION
from edrum.io.clock import FakeClock
from edrum.io.logfile import SessionWriter, read_log
from edrum.io.sources import (
    FakeSource,
    FileSource,
    LiveMidiSource,
    MessageStamper,
    Source,
    new_session_meta,
)
from tests.conftest import make_meta


def _live(clock=None) -> LiveMidiSource:
    return LiveMidiSource(
        "test-port", clock or FakeClock(), kit_profile_id="td02k", user_id="local"
    )


# ---------------------------------------------------------------------------
# MessageStamper: the capture edge
# ---------------------------------------------------------------------------

def test_note_on_becomes_event_with_arrival_stamp():
    clock = FakeClock(t_ms=1234)
    stamper = MessageStamper(clock)
    rec = stamper.stamp(mido.Message("note_on", channel=9, note=38, velocity=100))
    assert rec == EventRecord(t=1234, note=38, velocity=100, channel=9)


def test_realtime_messages_are_dropped():
    stamper = MessageStamper(FakeClock())
    for mtype in ("clock", "start", "continue", "stop", "active_sensing", "reset"):
        assert stamper.stamp(mido.Message(mtype)) is None


def test_note_off_and_velocity_zero_become_ctrl():
    """Micro-decision 3: preserved-but-invisible, never events, never dropped."""
    stamper = MessageStamper(FakeClock(t_ms=10))
    off = stamper.stamp(mido.Message("note_off", channel=9, note=42, velocity=64))
    assert isinstance(off, CtrlRecord)
    assert off.msg == {"type": "note_off", "channel": 9, "note": 42, "velocity": 64}
    v0 = stamper.stamp(mido.Message("note_on", channel=9, note=42, velocity=0))
    assert isinstance(v0, CtrlRecord)
    assert v0.msg["type"] == "note_on" and v0.msg["velocity"] == 0


def test_cc_and_aftertouch_and_sysex_become_ctrl():
    stamper = MessageStamper(FakeClock(t_ms=5))
    cc = stamper.stamp(mido.Message("control_change", channel=9, control=4, value=90))
    assert cc == CtrlRecord(
        t=5, msg={"type": "control_change", "channel": 9, "control": 4, "value": 90}
    )
    at = stamper.stamp(mido.Message("polytouch", channel=9, note=49, value=80))
    assert at.msg["type"] == "polytouch"
    syx = stamper.stamp(mido.Message("sysex", data=[65, 16, 0]))
    assert syx.msg == {"type": "sysex", "data": [65, 16, 0]}
    assert "time" not in cc.msg and "time" not in syx.msg  # t is authoritative


def test_stamp_reflects_arrival_time_not_processing_time():
    clock = FakeClock(t_ms=100)
    stamper = MessageStamper(clock)
    rec = stamper.stamp(mido.Message("note_on", channel=9, note=36, velocity=90))
    clock.advance_ms(500)  # anything after the stamp is too late to matter
    assert rec.t == 100


# ---------------------------------------------------------------------------
# meta construction
# ---------------------------------------------------------------------------

def test_new_session_meta():
    clock = FakeClock(start_iso="2026-07-06T12:00:00+08:00")
    meta = new_session_meta(clock, kit_profile_id="td02k", user_id="local")
    assert meta.schema_version == SCHEMA_VERSION
    assert meta.start_iso == "2026-07-06T12:00:00+08:00"
    assert meta.kit_profile_id == "td02k"
    assert meta.calibration_offset_ms is None  # null = uncalibrated (§6)
    assert len(meta.session_id) == 32


# ---------------------------------------------------------------------------
# source equivalence: the seam holds
# ---------------------------------------------------------------------------

def test_file_source_replays_what_fake_source_produced(tmp_path):
    """The brain cannot tell producers apart (spec §1 source-agnosticism)."""
    meta = make_meta()
    records = [
        EventRecord(t=10, note=36, velocity=90, channel=9),
        CtrlRecord(t=20, msg={"type": "control_change", "channel": 9, "control": 4, "value": 45}),
        EventRecord(t=30, note=38, velocity=70, channel=9),
    ]
    fake = FakeSource(meta, records)
    assert fake.open() == meta

    path = tmp_path / "s.jsonl"
    writer = SessionWriter(path, meta)
    for rec in fake.events():
        writer.append(rec)
    writer.close(end_t=40)

    fs = FileSource(path)
    assert fs.open() == meta
    replayed = list(fs.events())
    assert replayed[:-1] == records  # identical stream (+ the session_end)
    assert not fs.recovery.truncated


def test_all_sources_satisfy_protocol(tmp_path):
    assert isinstance(FakeSource(make_meta(), []), Source)
    assert isinstance(FileSource(tmp_path / "x.jsonl"), Source)
    assert isinstance(_live(), Source)


def test_file_source_requires_open_first(tmp_path):
    fs = FileSource(tmp_path / "x.jsonl")
    with pytest.raises(RuntimeError):
        list(fs.events())


# ---------------------------------------------------------------------------
# LiveMidiSource queue discipline (no ports: drive _on_message directly)
# ---------------------------------------------------------------------------

def test_live_source_stamps_on_callback_thread_and_drains_after_stop():
    clock = FakeClock()
    src = _live(clock)
    for t, note in ((100, 36), (200, 38), (300, 42)):
        clock.set_ms(t)
        src._on_message(mido.Message("note_on", channel=9, note=note, velocity=80))
    src.stop()  # stop BEFORE draining: events() must still yield everything
    got = list(src.events())
    assert [r.t for r in got] == [100, 200, 300]
    assert [r.note for r in got] == [36, 38, 42]


def test_live_source_filters_realtime_at_the_edge():
    src = _live()
    src._on_message(mido.Message("active_sensing"))
    src._on_message(mido.Message("clock"))
    src.stop()
    assert list(src.events()) == []


def test_slow_consumer_cannot_smear_stamps():
    """The ring-buffer property (capture spec §2): stamps happen at arrival,
    regardless of how slowly the storage side drains."""
    clock = FakeClock()
    src = _live(clock)
    stamped_ts = []

    def producer():
        for t in (10, 20, 30, 40, 50):
            clock.set_ms(t)
            src._on_message(mido.Message("note_on", channel=9, note=36, velocity=90))
            stamped_ts.append(t)
        src.stop()

    thread = threading.Thread(target=producer)
    thread.start()
    thread.join()

    drained = []
    for rec in src.events():
        time.sleep(0.01)  # deliberately slow consumer
        drained.append(rec.t)
    assert drained == stamped_ts  # order preserved, stamps untouched
