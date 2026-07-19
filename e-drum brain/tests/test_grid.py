"""S9′ grid geometry — property and boundary tests (phase1-stats-plan)."""

from __future__ import annotations

import math

from edrum.engine.grid import (
    beat_ms,
    beats_in,
    gridlines_in,
    nearest_gridline,
    segment_for,
    tick_ms,
)
from edrum.engine.session import GridSegment


def test_tick_and_beat_exact_values():
    assert tick_ms(120, 4) == 125.0
    assert tick_ms(120, 1) == 500.0
    assert tick_ms(60, 3) == 1000.0 / 3.0
    assert beat_ms(120) == 500.0
    assert beat_ms(90) == 60000.0 / 90.0


def test_deviation_range_property():
    """Every deviation lies in (-tick/2, +tick/2] — swept densely."""
    bpm, subdiv, downbeat = 120, 4, 1000
    tick = tick_ms(bpm, subdiv)
    for t in range(0, 10000, 7):  # off-phase sweep incl. t < downbeat
        g, _k = nearest_gridline(t, bpm, subdiv, downbeat)
        dev = t - g
        assert -tick / 2 < dev <= tick / 2, (t, dev)


def test_halfway_tie_grades_late_against_earlier_line():
    """t exactly between gridlines → earlier line, deviation +tick/2 (late)."""
    bpm, subdiv, downbeat = 120, 4, 1000  # tick 125, half 62.5
    g, k = nearest_gridline(1000 + 62.5, bpm, subdiv, downbeat)
    assert g == 1000.0 and k == 0
    assert (1000 + 62.5) - g == 62.5


def test_lattice_unbounded_and_negative_k():
    """Events before downbeat_t grade fine (8′: anchor may be after start_t)."""
    g, k = nearest_gridline(480, 120, 4, 1000)
    assert k == -4 and g == 500.0
    assert 480 - g == -20.0


def test_lattice_invariance_under_downbeat_shift():
    """Deviation is invariant when downbeat_t moves by whole ticks."""
    bpm, subdiv = 120, 4
    tick = tick_ms(bpm, subdiv)
    for t in (0, 333, 1042, 5991):
        base_g, base_k = nearest_gridline(t, bpm, subdiv, 1000)
        for shift in (-3, -1, 1, 8):
            g, k = nearest_gridline(t, bpm, subdiv, 1000 + shift * tick)
            assert math.isclose(t - g, t - base_g, abs_tol=1e-9)
            assert k == base_k - shift


def test_gridlines_in_matches_nearest_gridline():
    """Every enumerated gridline is its own nearest gridline (deviation 0)."""
    bpm, subdiv, downbeat = 90, 3, 700
    lines = gridlines_in(0, 5000, bpm, subdiv, downbeat)
    assert lines, "range must contain gridlines"
    for g in lines:
        ng, _k = nearest_gridline(g, bpm, subdiv, downbeat)
        assert math.isclose(ng, g, abs_tol=1e-9)
    # half-open: start included when on-line, end excluded
    lines2 = gridlines_in(700, 700 + 2 * tick_ms(bpm, subdiv), bpm, subdiv, downbeat)
    assert lines2[0] == 700.0 and len(lines2) == 2


def test_beats_in_are_subset_of_gridlines():
    bpm, subdiv, downbeat = 120, 4, 1000
    beats = beats_in(0, 4000, bpm, downbeat)
    lines = gridlines_in(0, 4000, bpm, subdiv, downbeat)
    for b in beats:
        assert any(math.isclose(b, g, abs_tol=1e-9) for g in lines)
    assert beats == [0.0, 500.0, 1000.0, 1500.0, 2000.0, 2500.0, 3000.0, 3500.0]


def _seg(start, end, bpm=120, subdiv=4, downbeat=None):
    return GridSegment(
        start_t=start, end_t=end, bpm=bpm, subdiv=subdiv,
        downbeat_t=downbeat if downbeat is not None else start,
    )


def test_segment_membership_inclusive_and_later_wins():
    a = _seg(1000, 2000, bpm=100)
    b = _seg(2000, 3000, bpm=140)  # param-change split shares t=2000
    track = [a, b]
    assert segment_for(999, track) is None
    assert segment_for(1000, track) is a
    assert segment_for(2000, track) is b  # later segment wins the boundary
    assert segment_for(3000, track) is b
    assert segment_for(3001, track) is None


def test_segment_for_empty_track_is_none():
    assert segment_for(1234, []) is None
