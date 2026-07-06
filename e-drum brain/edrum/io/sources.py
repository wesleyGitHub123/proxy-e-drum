"""S5 — the Source seam (brain spec §2): brain consumes live and file identically.

This abstraction is the load-bearing claim of the two-machine split: the brain
is source-agnostic, so "kit → laptop host" today and "kit → capture box" later
are interchangeable producers. The box's files arrive through ``FileSource``
unchanged — that is the entire hardware migration story.

``MessageStamper`` implements the capture edge (capture spec §3 Rule 2):
    * ``t`` is taken FIRST, before any classification or allocation
    * system-realtime messages are dropped (micro-decision 2)
    * note-on velocity>0 → ``event``; everything else (including note-off and
      note-on-velocity-0, micro-decision 3) → ``ctrl`` with the mido-style
      structured payload (micro-decision 1), ``time`` stripped

``LiveMidiSource`` stamps in the mido callback thread (the arrival edge) and
enqueues into a ``SimpleQueue`` — the laptop analog of the firmware ring
buffer (capture spec §2): a slow disk flush can never smear a timestamp.
"""

from __future__ import annotations

import threading
import uuid
from queue import Empty, SimpleQueue
from typing import Iterator, Protocol, runtime_checkable

from edrum.engine.records import (
    REALTIME_FILTER,
    SCHEMA_VERSION,
    CtrlRecord,
    EventRecord,
    MetaRecord,
    Record,
)
from edrum.io.clock import Clock
from edrum.io.logfile import LogRead, read_log


@runtime_checkable
class Source(Protocol):
    """What the capture pipeline may ask of a producer."""

    def open(self) -> MetaRecord: ...

    def events(self) -> Iterator[Record]: ...

    def close(self) -> None: ...


def new_session_meta(
    clock: Clock,
    *,
    kit_profile_id: str | None,
    user_id: str,
    calibration_offset_ms: int | float | None = None,
) -> MetaRecord:
    """Session meta at capture start. ``kit_profile_id`` is a reassignable
    pointer (spec §4.2) — a mislabel is re-pointed later, never re-recorded."""
    return MetaRecord(
        schema_version=SCHEMA_VERSION,
        session_id=uuid.uuid4().hex,
        start_iso=clock.start_iso,
        kit_profile_id=kit_profile_id,
        user_id=user_id,
        calibration_offset_ms=calibration_offset_ms,
    )


class MessageStamper:
    """Arrival-edge stamping + classification. One method, deliberately tiny:
    everything on this path runs inside the MIDI callback thread."""

    def __init__(self, clock: Clock) -> None:
        self._clock = clock

    def stamp(self, msg) -> Record | None:
        t = self._clock.now_ms()  # FIRST — before any other work (§3 Rule 2)
        if msg.type in REALTIME_FILTER:
            return None
        if msg.type == "note_on" and msg.velocity > 0:
            return EventRecord(t=t, note=msg.note, velocity=msg.velocity, channel=msg.channel)
        payload = msg.dict()
        payload.pop("time", None)  # our t is authoritative
        if "data" in payload:  # sysex bytes arrive tuple-like
            payload["data"] = [int(b) for b in payload["data"]]
        return CtrlRecord(t=t, msg=payload)


class FakeSource:
    """Scripted source — the test workhorse and fixture generator.

    Records are pre-stamped by the script; ``FakeSource`` just replays them,
    which is exactly the Source contract: the pipeline cannot tell it from a
    live kit.
    """

    def __init__(self, meta: MetaRecord, records: list[Record]) -> None:
        self._meta = meta
        self._records = list(records)

    def open(self) -> MetaRecord:
        return self._meta

    def events(self) -> Iterator[Record]:
        yield from self._records

    def close(self) -> None:
        pass


class FileSource:
    """Replay a session log — producer-agnostic by construction (spec §1)."""

    def __init__(self, path) -> None:
        self._path = path
        self._read: LogRead | None = None

    def open(self) -> MetaRecord:
        self._read = read_log(self._path)
        return self._read.meta

    def events(self) -> Iterator[Record]:
        if self._read is None:
            raise RuntimeError("FileSource.open() must be called before events()")
        yield from self._read.records

    def close(self) -> None:
        pass

    @property
    def recovery(self):
        if self._read is None:
            raise RuntimeError("FileSource.open() must be called before recovery")
        return self._read.recovery


class LiveMidiSource:
    """Kit → mido input port, wearing the capture hat (capture spec §4A).

    Threading model (mirrors firmware §2):
        * mido callback thread = capture edge: stamp, classify, enqueue —
          never blocks on storage
        * consumer (the capture pipeline) drains ``events()`` and writes

    Requires the ``[live]`` extra (python-rtmidi backend).
    """

    def __init__(
        self,
        port_name: str,
        clock: Clock,
        *,
        kit_profile_id: str | None,
        user_id: str,
        calibration_offset_ms: int | float | None = None,
    ) -> None:
        self._port_name = port_name
        self._clock = clock
        self._kit_profile_id = kit_profile_id
        self._user_id = user_id
        self._calibration_offset_ms = calibration_offset_ms
        self._stamper = MessageStamper(clock)
        self._queue: SimpleQueue = SimpleQueue()
        self._stop = threading.Event()
        self._port = None

    @property
    def clock(self) -> Clock:
        return self._clock

    def open(self) -> MetaRecord:
        import mido  # local import: engine/file tooling must not need a MIDI backend

        self._port = mido.open_input(self._port_name, callback=self._on_message)
        return new_session_meta(
            self._clock,
            kit_profile_id=self._kit_profile_id,
            user_id=self._user_id,
            calibration_offset_ms=self._calibration_offset_ms,
        )

    def _on_message(self, msg) -> None:
        """Runs on the MIDI backend thread — the capture edge."""
        record = self._stamper.stamp(msg)
        if record is not None:
            self._queue.put(record)

    def events(self) -> Iterator[Record]:
        """Yields until ``stop()``; drains the queue fully before returning
        (never drop a captured event — capture spec §2)."""
        while not (self._stop.is_set() and self._queue.empty()):
            try:
                yield self._queue.get(timeout=0.05)
            except Empty:
                continue

    def stop(self) -> None:
        self._stop.set()

    def close(self) -> None:
        self._stop.set()
        if self._port is not None:
            self._port.close()
            self._port = None
