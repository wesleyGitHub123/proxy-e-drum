"""Hit density — practice-volume evidence (a Layer-D covariate later).

Also the standing proof of the extensibility contract (phase1-stats-plan
D.6.5): this module was added end-to-end by writing this file and one line
in the registry — no runner, CLI, or report changes. Any future analyzer
should cost exactly this much.
"""

from __future__ import annotations

from edrum.engine.analysis import (
    AnalysisInputs,
    Analyzer,
    MetricResult,
    Quality,
    Requirements,
    Scope,
)


def _hits_per_minute(inputs: AnalysisInputs, _upstream) -> list[MetricResult]:
    included = [ee for ee in inputs.events if not ee.excluded]
    session = inputs.session
    # Duration: session_end when written, else the last event (the crash-
    # recovery shape). Zero/unknown duration → None, never a fake rate.
    duration_ms = session.end_t
    if duration_ms is None and session.events:
        duration_ms = session.events[-1].t
    value = (
        60000.0 * len(included) / duration_ms
        if duration_ms is not None and duration_ms > 0
        else None
    )
    return [
        MetricResult(
            analyzer_id="density.hits_per_minute",
            analyzer_version=1,
            scope=Scope(),
            unit="hits/min",
            value=value,
            quality=Quality(
                n=len(included), n_excluded=len(inputs.events) - len(included)
            ),
        )
    ]


HITS_PER_MINUTE = Analyzer(
    id="density.hits_per_minute",
    version=1,
    requires=Requirements(),
    compute=_hits_per_minute,
)
