"""S4 — the one clock (capture spec §3 Rule 1, §4A).

The single time authority of the laptop-phase capture half. MIDI arrival is
stamped against it (in the mido callback — arrival edge, Rule 2) and the
Phase 1 click scheduler will consume the *same instance* — that shared
instance is the one-clock rule, enforced by construction.

``t`` is session-relative integer milliseconds, monotonic, and NEVER rebased
(brain spec §3): auto-pause is an annotation on the timeline, not a clock stop.
There is deliberately no pause/reset API on these clocks.
"""

from __future__ import annotations

import time
from datetime import datetime
from typing import Protocol, runtime_checkable


@runtime_checkable
class Clock(Protocol):
    """What a capture-half component may ask of time."""

    @property
    def start_iso(self) -> str: ...

    def now_ms(self) -> int: ...


class SessionClock:
    """Real clock: wall anchor at construction + monotonic perf counter."""

    def __init__(self) -> None:
        self._start_dt = datetime.now().astimezone()
        self._anchor_ns = time.perf_counter_ns()

    @property
    def start_dt(self) -> datetime:
        return self._start_dt

    @property
    def start_iso(self) -> str:
        return self._start_dt.isoformat(timespec="seconds")

    def now_ms(self) -> int:
        return (time.perf_counter_ns() - self._anchor_ns) // 1_000_000


class FakeClock:
    """Injectable deterministic clock.

    Part of the testability contract (phase0-plan S4), not test-only code:
    FakeSource-driven pipelines use it to produce reproducible session files
    (golden fixtures).
    """

    def __init__(self, start_iso: str = "2026-01-01T00:00:00+00:00", t_ms: int = 0) -> None:
        self._start_iso = start_iso
        self._t = int(t_ms)

    @property
    def start_dt(self) -> datetime:
        return datetime.fromisoformat(self._start_iso)

    @property
    def start_iso(self) -> str:
        return self._start_iso

    def now_ms(self) -> int:
        return self._t

    def advance_ms(self, delta: int) -> None:
        if delta < 0:
            raise ValueError("clock never goes backwards")
        self._t += int(delta)

    def set_ms(self, t: int) -> None:
        if t < self._t:
            raise ValueError("clock never goes backwards")
        self._t = int(t)
