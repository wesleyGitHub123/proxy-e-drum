"""Velocity analyzers — spec §6 Layer A, grid-free tier.

Valid over any span, grid or no grid (velocity needs no reference).
Raw-note stats are the always-valid primary (micro-decision 15); lane stats
are profile-dependent VIEWS — re-pointing the kit profile changes lane-scoped
results without touching note-scoped ones (the SSD property, spec §4.2).

Velocity is "raw as received" — post-kit-firmware, the module's own velocity
curve baked in (spec §4.2 honesty note). Cross-kit comparability is a Phase 6
calibration, not a claim these numbers make.
"""

from __future__ import annotations

import statistics
from dataclasses import dataclass

from edrum.engine.analysis import (
    AnalysisInputs,
    Analyzer,
    MeanStd,
    MetricResult,
    Quality,
    Requirements,
    Scope,
)


@dataclass(frozen=True)
class VelocityRow:
    t: int
    note: int
    velocity: int
    lane: str | None
    articulation: str | None
    excluded: bool


def _mean_std(values: list[int | float]) -> MeanStd:
    return MeanStd(
        mean=statistics.fmean(values) if values else None,
        std=statistics.stdev(values) if len(values) >= 2 else None,
    )


def _observations(inputs: AnalysisInputs, _upstream) -> list[MetricResult]:
    rows = tuple(
        VelocityRow(
            t=ee.t,
            note=ee.note,
            velocity=ee.velocity,
            lane=ee.lane,
            articulation=ee.articulation,
            excluded=ee.excluded,
        )
        for ee in inputs.events
    )
    n_excluded = sum(1 for r in rows if r.excluded)
    return [
        MetricResult(
            analyzer_id="velocity.observations",
            analyzer_version=1,
            scope=Scope(),
            unit="velocity",
            value=rows,
            quality=Quality(n=len(rows) - n_excluded, n_excluded=n_excluded),
        )
    ]


OBSERVATIONS = Analyzer(
    id="velocity.observations",
    version=1,
    requires=Requirements(),
    compute=_observations,
)


def _per_note(_inputs: AnalysisInputs, upstream) -> list[MetricResult]:
    (obs,) = upstream["velocity.observations"]
    by_note: dict[int, list[int]] = {}
    for r in obs.value:
        if not r.excluded:
            by_note.setdefault(r.note, []).append(r.velocity)
    return [
        MetricResult(
            analyzer_id="velocity.per_note",
            analyzer_version=1,
            scope=Scope(note=note),
            unit="velocity",
            value=_mean_std(by_note[note]),
            quality=Quality(n=len(by_note[note])),
        )
        for note in sorted(by_note)
    ]


PER_NOTE = Analyzer(
    id="velocity.per_note",
    version=1,
    requires=Requirements(upstream=("velocity.observations",)),
    compute=_per_note,
)


def _per_lane(_inputs: AnalysisInputs, upstream) -> list[MetricResult]:
    (obs,) = upstream["velocity.observations"]
    by_lane: dict[tuple[str, str | None], list[int]] = {}
    for r in obs.value:
        if not r.excluded and r.lane is not None:
            by_lane.setdefault((r.lane, r.articulation), []).append(r.velocity)
    return [
        MetricResult(
            analyzer_id="velocity.per_lane",
            analyzer_version=1,
            scope=Scope(lane=lane, articulation=articulation),
            unit="velocity",
            value=_mean_std(by_lane[(lane, articulation)]),
            quality=Quality(n=len(by_lane[(lane, articulation)])),
        )
        for lane, articulation in sorted(by_lane, key=lambda k: (k[0], k[1] or ""))
    ]


PER_LANE = Analyzer(
    id="velocity.per_lane",
    version=1,
    requires=Requirements(needs_profile=True, upstream=("velocity.observations",)),
    compute=_per_lane,
)
