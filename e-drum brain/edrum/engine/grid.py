"""S9′ — Grid geometry: the single source of lattice truth, brain-side.

Micro-decisions 8′, 9, 12 (phase1-stats-plan Part C):

    * The lattice is PER SEGMENT: ``tick_ms = 60000 / bpm / subdiv``;
      gridlines are ``downbeat_t + k * tick_ms`` for ALL integers k —
      unbounded in both directions *within that segment's own arithmetic*.
      A grid segment bounds which *events* are graded, never which lattice
      points exist. A session is a chain of independently-anchored lattices
      (one per segment), NOT one session-wide grid: each tempo/subdiv change
      re-declares the span with a fresh ``downbeat_t``, and beat phase /
      lattice index ``k`` are meaningful only relative to one segment's
      anchor — phase continuity ACROSS a segment boundary is undefined
      (the producer's click restarts from a new anchor on every param
      change; no relationship between adjacent segments' ``k`` exists).
      Never pool ``k`` or fold beat-relative positions across segments as
      if they shared a phase origin.
    * ``downbeat_t`` is a BEAT edge of the running click **as written by the
      producer** (8′). The firmware anchors declarations at the first beat
      edge at or after the declaration (``ClickScheduler::next_edge_at_or_after``),
      so ``downbeat_t`` may legitimately be AFTER a segment's ``start_t``;
      nothing here may assume otherwise. Beat-anchoring (never
      subdivision-anchoring) is the load-bearing property — it preserves beat
      phase *within the segment* for Layer C folding.
    * ``subdiv`` = ticks per beat: 1=quarters, 2=8ths, 3=8th-triplets, 4=16ths.
    * Segment membership is inclusive ``[start_t, end_t]``; where two segments
      share a boundary t, the LATER segment wins (the param change takes
      effect at the change point) — micro-decision 9.
    * One lattice truth (12): no ``60000 /`` exists anywhere outside this
      module. The firmware's ``ClickScheduler`` is the producer-side twin;
      golden fixtures keep the two honest.

Pure arithmetic, no state, no I/O. Times are session-relative ms (ints from
records; floats once lattice math is involved).
"""

from __future__ import annotations

import math

from edrum.engine.session import GridSegment


def tick_ms(bpm: int | float, subdiv: int) -> float:
    """Grid tick period: subdivision spacing in ms."""
    return 60000.0 / bpm / subdiv


def beat_ms(bpm: int | float) -> float:
    """Beat period in ms."""
    return 60000.0 / bpm


def nearest_gridline(
    t: int | float, bpm: int | float, subdiv: int, downbeat_t: int | float
) -> tuple[float, int]:
    """The lattice point nearest ``t``: returns ``(gridline_t, k)``.

    ``k`` is the (possibly negative) lattice index relative to ``downbeat_t``.
    Deviation ``t - gridline_t`` is in ``(-tick/2, +tick/2]``: an event
    exactly halfway between two gridlines grades against the EARLIER line
    (deviation +tick/2 — maximally late), fixing the tie deterministically
    with the sign convention of micro-decision 7 (positive = late/drag).
    """
    tick = tick_ms(bpm, subdiv)
    k = math.ceil((t - downbeat_t) / tick - 0.5)
    return downbeat_t + k * tick, k


def gridlines_in(
    start_t: int | float,
    end_t: int | float,
    bpm: int | float,
    subdiv: int,
    downbeat_t: int | float,
) -> list[float]:
    """All gridline times in the half-open range ``[start_t, end_t)``."""
    tick = tick_ms(bpm, subdiv)
    k = math.ceil((start_t - downbeat_t) / tick)
    out: list[float] = []
    g = downbeat_t + k * tick
    while g < end_t:
        out.append(g)
        k += 1
        g = downbeat_t + k * tick
    return out


def beats_in(
    start_t: int | float,
    end_t: int | float,
    bpm: int | float,
    downbeat_t: int | float,
) -> list[float]:
    """All beat times in ``[start_t, end_t)`` (Layer C's beat-relative hook)."""
    beat = beat_ms(bpm)
    j = math.ceil((start_t - downbeat_t) / beat)
    out: list[float] = []
    b = downbeat_t + j * beat
    while b < end_t:
        out.append(b)
        j += 1
        b = downbeat_t + j * beat
    return out


def segment_for(t: int | float, grid_track: list[GridSegment]) -> GridSegment | None:
    """The segment grading an event at ``t``, or None (free play).

    Membership is inclusive; at a shared boundary the later segment wins
    (micro-decision 9). ``grid_track`` is in declaration order (the fold's
    output), so "later" = last match.
    """
    match: GridSegment | None = None
    for seg in grid_track:
        if seg.start_t <= t <= seg.end_t:
            match = seg
    return match
