"""The analyzer registry — explicit, lightweight, and the ONLY file that
changes when a new metric is added (micro-decision 19).

To add an analyzer months from now:
    1. write a new module in this package: a pure ``compute`` function plus
       an ``Analyzer`` descriptor declaring its ``Requirements`` (data
       preconditions + upstream analyzer ids);
    2. import it below and append it to ``ALL``.

Nothing else changes — the runner orders it by its declared dependencies,
the CLI and ``--json`` render it through the uniform envelope, and Layer D
will pick its results up by ``(analyzer_id, scope)``. There is deliberately
no dynamic discovery, no entry points, and no configuration surface: the
registry is code, reviewed like code (Part B.1's anti-framework stance).
"""

from __future__ import annotations

from edrum.engine.analysis import Analyzer
from edrum.engine.analyzers.density import HITS_PER_MINUTE
from edrum.engine.analyzers.ioi import PAIRS, SUMMARY
from edrum.engine.analyzers.timing import CONSISTENCY, DEVIATIONS, RUSH_DRAG
from edrum.engine.analyzers.velocity import OBSERVATIONS, PER_LANE, PER_NOTE

#: Registry order = evaluation preference (the runner still topo-sorts by
#: declared upstream, so order here is cosmetic for the report layout).
ALL: tuple[Analyzer, ...] = (
    DEVIATIONS,
    RUSH_DRAG,
    CONSISTENCY,
    OBSERVATIONS,
    PER_NOTE,
    PER_LANE,
    PAIRS,
    SUMMARY,
    HITS_PER_MINUTE,
)

_BY_ID = {a.id: a for a in ALL}
assert len(_BY_ID) == len(ALL), "duplicate analyzer id in registry"


def by_id(analyzer_id: str) -> Analyzer | None:
    """Look up a registered analyzer (None if unknown)."""
    return _BY_ID.get(analyzer_id)
