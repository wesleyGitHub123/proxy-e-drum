"""Timing analyzers — spec §6 Layer A, the grid-bound tier.

``timing.deviations`` (Tier 1) is the geometry: one row per graded event,
drawable directly on the piano roll (event ↔ gridline ↔ signed deviation —
spec §1A). ``timing.rush_drag`` and ``timing.consistency`` (Tier 2) are
compressions of that table; they contain no lattice math and never touch the
session directly — everything flows through the declared upstream.

Semantics (phase1-stats-plan Part C):
    * micro-decision 7 — ``t_corrected = t − calibration_offset_ms`` (null →
      0 + uncalibrated flag); assignment AND deviation use ``t_corrected``;
      positive = late (drag), negative = early (rush).
    * Segment membership uses RAW ``t`` (micro-decision 9): boundaries are
      declarations stamped on the same clock as the events; calibration
      corrects grading geometry, not which span a hit belongs to.
    * micro-decision 10 — ddof=1; ``None`` under n<2, never a fake 0.0;
      ambiguity ``|dev| > 0.35·tick`` counted and still included.
    * Excluded (gesture trigger) rows are marked and carried in the table,
      but never counted in n or aggregates (micro-decision 18).
"""

from __future__ import annotations

import statistics
from dataclasses import dataclass

from edrum.engine.analysis import (
    AMBIGUITY_FRACTION,
    AnalysisInputs,
    Analyzer,
    MetricResult,
    Quality,
    Requirements,
    Scope,
    SpanRef,
)
from edrum.engine.grid import nearest_gridline, segment_for, tick_ms


@dataclass(frozen=True)
class DeviationRow:
    """One graded event: the drawable geometry (spec §1A)."""

    t: int
    t_corrected: float
    note: int
    velocity: int
    lane: str | None
    articulation: str | None
    gridline_t: float
    k: int
    deviation_ms: float
    ambiguous: bool
    excluded: bool


def _segment_scope(seg) -> Scope:
    return Scope(
        span=SpanRef(kind="grid", start_t=seg.start_t, end_t=seg.end_t),
        bpm=seg.bpm,
        subdiv=seg.subdiv,
    )


def _deviations(inputs: AnalysisInputs, _upstream) -> list[MetricResult]:
    session = inputs.session
    cal = inputs.calibration_offset_ms if inputs.calibration_offset_ms is not None else 0
    flags = () if inputs.calibrated else ("uncalibrated",)

    rows_by_seg: dict[int, list[DeviationRow]] = {id(seg): [] for seg in session.grid_track}
    for ee in inputs.events:
        seg = segment_for(ee.t, session.grid_track)
        if seg is None:
            continue  # free play — truthfully ungraded
        t_corrected = ee.t - cal
        gridline_t, k = nearest_gridline(t_corrected, seg.bpm, seg.subdiv, seg.downbeat_t)
        deviation = t_corrected - gridline_t
        tick = tick_ms(seg.bpm, seg.subdiv)
        rows_by_seg[id(seg)].append(
            DeviationRow(
                t=ee.t,
                t_corrected=t_corrected,
                note=ee.note,
                velocity=ee.velocity,
                lane=ee.lane,
                articulation=ee.articulation,
                gridline_t=gridline_t,
                k=k,
                deviation_ms=deviation,
                ambiguous=abs(deviation) > AMBIGUITY_FRACTION * tick,
                excluded=ee.excluded,
            )
        )

    results = []
    for seg in session.grid_track:
        rows = rows_by_seg[id(seg)]
        included = [r for r in rows if not r.excluded]
        results.append(
            MetricResult(
                analyzer_id="timing.deviations",
                analyzer_version=1,
                scope=_segment_scope(seg),
                unit="ms",
                value=tuple(rows),
                quality=Quality(
                    n=len(included),
                    n_excluded=len(rows) - len(included),
                    n_ambiguous=sum(1 for r in included if r.ambiguous),
                    calibrated=inputs.calibrated,
                    flags=flags,
                ),
            )
        )
    return results


DEVIATIONS = Analyzer(
    id="timing.deviations",
    version=1,
    requires=Requirements(needs_grid=True),
    compute=_deviations,
)


# ---------------------------------------------------------------------------
# Tier 2 — compressions of the deviation table
# ---------------------------------------------------------------------------

def _grouped(dev_results):
    """Yield (scope, included_rows, n_excluded, n_ambiguous) for every
    grouping micro-decision 15 asks for: per segment, per raw note, per
    (lane, articulation) when a profile supplied lanes, plus pooled per
    exact (bpm, subdiv) across segments."""
    pooled: dict[tuple, list[DeviationRow]] = {}
    pooled_excl: dict[tuple, int] = {}
    for res in dev_results:
        rows = list(res.value)
        included = [r for r in rows if not r.excluded]
        n_excl = len(rows) - len(included)
        yield res.scope, included, n_excl

        by_note: dict[int, list[DeviationRow]] = {}
        by_lane: dict[tuple[str, str | None], list[DeviationRow]] = {}
        for r in included:
            by_note.setdefault(r.note, []).append(r)
            if r.lane is not None:
                by_lane.setdefault((r.lane, r.articulation), []).append(r)
        for note in sorted(by_note):
            yield (
                Scope(span=res.scope.span, bpm=res.scope.bpm, subdiv=res.scope.subdiv, note=note),
                by_note[note],
                0,
            )
        for lane, articulation in sorted(by_lane, key=lambda k: (k[0], k[1] or "")):
            yield (
                Scope(
                    span=res.scope.span,
                    bpm=res.scope.bpm,
                    subdiv=res.scope.subdiv,
                    lane=lane,
                    articulation=articulation,
                ),
                by_lane[(lane, articulation)],
                0,
            )

        key = (res.scope.bpm, res.scope.subdiv)
        pooled.setdefault(key, []).extend(included)
        pooled_excl[key] = pooled_excl.get(key, 0) + n_excl

    for bpm, subdiv in sorted(pooled):
        yield Scope(bpm=bpm, subdiv=subdiv), pooled[(bpm, subdiv)], pooled_excl[(bpm, subdiv)]


def _compress(analyzer_id: str, aggregate, min_n: int):
    """Build a Tier-2 compute fn: aggregate(list of deviations) over every
    grouping; value None (never fake) below min_n. Quality (calibrated,
    uncalibrated flag) is inherited from upstream by the runner's meet."""

    def _compute(_inputs: AnalysisInputs, upstream) -> list[MetricResult]:
        out = []
        for scope, rows, n_excl in _grouped(upstream["timing.deviations"]):
            devs = [r.deviation_ms for r in rows]
            out.append(
                MetricResult(
                    analyzer_id=analyzer_id,
                    analyzer_version=1,
                    scope=scope,
                    unit="ms",
                    value=aggregate(devs) if len(devs) >= min_n else None,
                    quality=Quality(
                        n=len(rows),
                        n_excluded=n_excl,
                        n_ambiguous=sum(1 for r in rows if r.ambiguous),
                    ),
                )
            )
        return out

    return _compute


RUSH_DRAG = Analyzer(
    id="timing.rush_drag",
    version=1,
    requires=Requirements(upstream=("timing.deviations",)),
    compute=_compress("timing.rush_drag", statistics.fmean, min_n=1),
)

CONSISTENCY = Analyzer(
    id="timing.consistency",
    version=1,
    requires=Requirements(upstream=("timing.deviations",)),
    compute=_compress("timing.consistency", statistics.stdev, min_n=2),
)
