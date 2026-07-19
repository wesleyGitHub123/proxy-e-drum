"""Inter-onset-interval analyzers — spec §6 Layer A, grid-free tier.

IOI is descriptive rhythm evidence: valid over any span, no grid required,
and the input Layer B's subdivision inference will cluster later.

Pairing rule (micro-decision 18 applied to intervals): a pair joins
consecutive INCLUDED members of a stream; a pair is dropped (counted in
``n_excluded``) when an excluded member of the SAME stream lies between its
endpoints — a kick-to-kick interval spanning an excluded gesture kick is not
a real stroke interval. Excluded members of other streams do not break a
pair: two snare onsets around a kick+crash gesture burst are still
consecutive snare onsets. ``t`` is monotonic session wall-time and never
rebased (spec §3), so IOIs across an auto-pause are correct by construction.
"""

from __future__ import annotations

import statistics
from dataclasses import dataclass

from edrum.engine.analysis import (
    AnalysisInputs,
    Analyzer,
    EffectiveEvent,
    MeanStd,
    MetricResult,
    Quality,
    Requirements,
    Scope,
)


@dataclass(frozen=True)
class IoiRow:
    t0: int
    t1: int
    ioi_ms: int
    note0: int
    note1: int


def _pairs_in(stream: list[EffectiveEvent]) -> tuple[list[IoiRow], int]:
    """Pair consecutive included members; drop pairs bridging an excluded
    member of this stream. Returns (rows, dropped_count)."""
    rows: list[IoiRow] = []
    dropped = 0
    prev: EffectiveEvent | None = None
    barrier = False
    for ee in stream:
        if ee.excluded:
            barrier = prev is not None
            continue
        if prev is not None:
            if barrier:
                dropped += 1
            else:
                rows.append(
                    IoiRow(
                        t0=prev.t,
                        t1=ee.t,
                        ioi_ms=ee.t - prev.t,
                        note0=prev.note,
                        note1=ee.note,
                    )
                )
        prev = ee
        barrier = False
    return rows, dropped


def _pairs(inputs: AnalysisInputs, _upstream) -> list[MetricResult]:
    def result(scope: Scope, stream: list[EffectiveEvent]) -> MetricResult:
        rows, dropped = _pairs_in(stream)
        return MetricResult(
            analyzer_id="ioi.pairs",
            analyzer_version=1,
            scope=scope,
            unit="ms",
            value=tuple(rows),
            quality=Quality(n=len(rows), n_excluded=dropped),
        )

    out = [result(Scope(), list(inputs.events))]  # pooled over the session
    by_note: dict[int, list[EffectiveEvent]] = {}
    for ee in inputs.events:
        by_note.setdefault(ee.note, []).append(ee)
    for note in sorted(by_note):
        out.append(result(Scope(note=note), by_note[note]))
    return out


PAIRS = Analyzer(
    id="ioi.pairs",
    version=1,
    requires=Requirements(),
    compute=_pairs,
)


def _summary(_inputs: AnalysisInputs, upstream) -> list[MetricResult]:
    out = []
    for res in upstream["ioi.pairs"]:
        iois = [row.ioi_ms for row in res.value]
        out.append(
            MetricResult(
                analyzer_id="ioi.summary",
                analyzer_version=1,
                scope=res.scope,
                unit="ms",
                value=MeanStd(
                    mean=statistics.fmean(iois) if iois else None,
                    std=statistics.stdev(iois) if len(iois) >= 2 else None,
                ),
                quality=Quality(n=len(iois), n_excluded=res.quality.n_excluded),
            )
        )
    return out


SUMMARY = Analyzer(
    id="ioi.summary",
    version=1,
    requires=Requirements(upstream=("ioi.pairs",)),
    compute=_summary,
)
