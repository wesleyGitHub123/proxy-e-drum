"""S4 tests: the one clock."""

from __future__ import annotations

import pytest

from edrum.io.clock import Clock, FakeClock, SessionClock


def test_session_clock_monotonic_integer_ms():
    clock = SessionClock()
    a = clock.now_ms()
    b = clock.now_ms()
    assert isinstance(a, int) and isinstance(b, int)
    assert 0 <= a <= b


def test_session_clock_start_iso_carries_offset():
    clock = SessionClock()
    # absolute dating for longitudinal ordering (spec 3a) needs tz-aware iso
    assert "T" in clock.start_iso
    assert clock.start_dt.tzinfo is not None


def test_fake_clock_deterministic_and_never_rebased():
    clock = FakeClock(t_ms=100)
    assert clock.now_ms() == 100
    clock.advance_ms(50)
    assert clock.now_ms() == 150
    clock.set_ms(400)
    assert clock.now_ms() == 400
    with pytest.raises(ValueError):
        clock.set_ms(10)  # t never freezes, never rewinds (spec §3)
    with pytest.raises(ValueError):
        clock.advance_ms(-1)


def test_clocks_satisfy_protocol():
    assert isinstance(SessionClock(), Clock)
    assert isinstance(FakeClock(), Clock)


def test_no_pause_api():
    # auto-pause is an annotation, never a clock stop (spec §3): the clock
    # deliberately exposes no pause/reset surface.
    for cls in (SessionClock, FakeClock):
        assert not any("pause" in name or "reset" in name for name in dir(cls))
