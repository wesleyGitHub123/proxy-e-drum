"""S14 — Analysis core: the result envelope, the analyzer contract, the runner.

This is the seam between parsing and analysis (phase1-stats-plan Part B).
Analyzers are plain pure functions wrapped in a small frozen descriptor —
deliberately NOT a framework: no dynamic discovery, no config surface, no
base-class hierarchy. Adding a metric is one new module plus one line in the
explicit registry list (``edrum/engine/analyzers/__init__.py``), and nothing
else in the system changes (micro-decision 19).

Three load-bearing ideas:

    * **Dependency declaration does triple duty** — ``Requirements`` is the
      DAG edge set for ordering, the claim-bounding gate (an unsatisfiable
      requirement yields a structured ``Refusal``, never a fake number and
      never a crash — spec §0), and the provenance record.
    * **The envelope is uniform** — every result is a ``MetricResult`` with a
      flat-axes ``Scope`` and a ``Quality`` block, so the CLI, future
      visualizations, and Layer D consume results without knowing any
      analyzer's internals. Result *shapes* stay unfrozen (micro-decision 11);
      golden VALUES freeze the numbers.
    * **Quality is plumbing, not discipline** — the runner meets each
      result's quality with everything its analyzer consumed upstream
      (micro-decision 17), so a derived metric cannot forget to inherit its
      inputs' uncertainty.

Results are never stored anywhere (spec §1A invariant 2): this module
computes, returns, and forgets. Every provenance component a future
rebuildable cache would need is already stamped on every result (Part B.8).
"""

from __future__ import annotations

from dataclasses import dataclass, replace
from typing import Callable, Mapping, Sequence

from edrum.engine.normalize import normalize_event
from edrum.engine.profiles import KitProfile
from edrum.engine.records import EventRecord
from edrum.engine.session import Session

#: Engine-wide analysis version (micro-decision 11): bumps whenever any
#: golden value changes, so a Layer-D discontinuity is always attributable.
#: Per-analyzer ``version`` fields attribute the change to its source.
ANALYSIS_VERSION = 1

#: Micro-decision 10 / D3: |deviation| > this fraction of a tick is counted
#: ambiguous — the honest signal that nearest-line grading is breaking down
#: on this material (Layer B's cue). Included in stats, surfaced in Quality.
AMBIGUITY_FRACTION = 0.35


# ---------------------------------------------------------------------------
# the envelope (micro-decision 16)
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class SpanRef:
    """A span axis value: which declared span a result describes."""

    kind: str  # "grid" | "enroll"
    start_t: int
    end_t: int


@dataclass(frozen=True)
class Scope:
    """What a result describes — FLAT typed axes, deliberately not nested.

    Flatness is what makes cross-session alignment mechanical: Layer D
    groups results by ``(analyzer_id, axes...)`` with no per-analyzer
    knowledge. All axes default to None; a session-wide metric leaves every
    axis unset. ``grid_source`` is reserved for Layer B (None = declared
    grid; inference will emit additional results under "inferred", never
    replacing declared-grid rows — Part B.9's isolation guarantee).
    """

    span: SpanRef | None = None
    lane: str | None = None  # Instrument.value via the kit profile
    articulation: str | None = None
    note: int | None = None  # raw note (always-valid axis; lane is a view)
    bpm: int | float | None = None
    subdiv: int | None = None
    grid_source: str | None = None


@dataclass(frozen=True)
class Quality:
    """Claim support. ``confidence=None`` means deterministic — exact given
    the data; only inference analyzers (Layer B+) ever set it.
    ``calibrated=None`` means calibration is not applicable to this metric
    (e.g. velocity); timing metrics set True/False from session meta."""

    n: int
    n_excluded: int = 0
    n_ambiguous: int = 0
    calibrated: bool | None = None
    confidence: float | None = None
    flags: tuple[str, ...] = ()


@dataclass(frozen=True)
class MetricResult:
    analyzer_id: str
    analyzer_version: int
    scope: Scope
    unit: str  # "ms", "velocity", ... — presentation hint, not semantics
    value: object  # float | int | None | tuple of row dataclasses | stats dataclass
    quality: Quality


@dataclass(frozen=True)
class MeanStd:
    """A common Tier-2 value: honest mean/spread. ``std=None`` under n<2 and
    ``mean=None`` under n<1 — never a fake zero (micro-decision 10)."""

    mean: float | None
    std: float | None


@dataclass(frozen=True)
class Refusal:
    """A structured non-answer: the data can't support this claim (spec §0).

    A Refusal is a *result*, not an error — the CLI prints it as
    information and exits 0 (absence of grid is truth, not failure)."""

    analyzer_id: str
    reason: str  # "no_grid_segments" | "no_profile" | "upstream_refused"
    detail: str = ""


@dataclass(frozen=True)
class Requirements:
    """Declarative preconditions + upstream analyzer dependencies."""

    needs_grid: bool = False
    needs_profile: bool = False
    upstream: tuple[str, ...] = ()


@dataclass(frozen=True)
class Analyzer:
    """A pure metric computation with identity, version, and declared deps.

    ``compute(inputs, upstream)`` receives the substrate and a mapping
    {analyzer_id: (MetricResult, ...)} for each declared upstream, and
    returns this analyzer's results. It must be deterministic, side-effect
    free, and iterate events in t-order (float determinism)."""

    id: str
    version: int
    requires: Requirements
    compute: Callable[
        ["AnalysisInputs", Mapping[str, tuple[MetricResult, ...]]],
        Sequence[MetricResult],
    ]


# ---------------------------------------------------------------------------
# the substrate (Part B.3) — built once, immutable, shared by all analyzers
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class EffectiveEvent:
    """One performance-track event with its analysis marks.

    ``excluded`` = inside a declared gesture trigger span (micro-decision
    18): a mark on a view, never a deletion — the raw event is untouched and
    Tier-1 tables carry the mark through so the geometry stays drawable.
    ``lane``/``articulation`` are the profile-derived view (None without a
    profile — raw ``note`` is always the primary key, spec §4.2)."""

    event: EventRecord
    excluded: bool
    lane: str | None
    articulation: str | None

    @property
    def t(self) -> int:
        return self.event.t

    @property
    def note(self) -> int:
        return self.event.note

    @property
    def velocity(self) -> int:
        return self.event.velocity


@dataclass(frozen=True)
class AnalysisInputs:
    """Everything an analyzer may see. Analyzers get this and upstream
    results — no paths, no files, no registry access."""

    session: Session
    profile: KitProfile | None
    events: tuple[EffectiveEvent, ...]  # t-order, marks applied
    calibration_offset_ms: int | float | None
    calibrated: bool


def build_inputs(session: Session, profile: KitProfile | None = None) -> AnalysisInputs:
    """Assemble the substrate: exclusion marks + lane views, computed once."""
    spans = tuple(session.trigger_spans)

    def _excluded(t: int) -> bool:
        return any(t0 <= t <= t1 for t0, t1 in spans)

    events = []
    for ev in session.events:
        lane = articulation = None
        if profile is not None:
            view = normalize_event(ev, profile)
            lane = view.instrument.value
            articulation = view.articulation
        events.append(
            EffectiveEvent(
                event=ev, excluded=_excluded(ev.t), lane=lane, articulation=articulation
            )
        )
    cal = session.meta.calibration_offset_ms
    return AnalysisInputs(
        session=session,
        profile=profile,
        events=tuple(events),
        calibration_offset_ms=cal,
        calibrated=cal is not None,
    )


# ---------------------------------------------------------------------------
# the report
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class AnalysisReport:
    """One session's full analysis: provenance header + results + refusals.

    This is Layer D's time-series atom. Never persisted by the engine."""

    session_id: str
    schema_version: int
    analysis_version: int
    kit_profile_id: str | None  # meta's (possibly stale) pointer
    profile_id: str | None  # the profile actually used (re-pointable, §4.2)
    profile_version: int | None
    calibrated: bool
    calibration_offset_ms: int | float | None
    warnings: tuple[str, ...]
    results: tuple[MetricResult, ...]
    refusals: tuple[Refusal, ...]


def _looks_undated(start_iso: str) -> bool:
    """Pre-`settime` cold-boot sessions carry epoch dates (ADR-5). Flag them
    so Layer D never orders a trend on a fictional axis."""
    try:
        return int(start_iso[:4]) < 2000
    except (ValueError, IndexError):
        return True


# ---------------------------------------------------------------------------
# the runner
# ---------------------------------------------------------------------------

def _resolve(analyzers: Sequence[Analyzer]) -> list[Analyzer]:
    """Close over upstream deps (from the registry) and topologically order.

    A missing upstream id or a cycle is a programming error in registered
    code, not a data condition — both raise."""
    from edrum.engine.analyzers import by_id  # lazy: avoids import cycle

    # Seed with everything explicitly requested first, so a passed-in
    # dependency satisfies its dependent regardless of list order; only
    # then pull missing upstreams from the registry.
    chosen: dict[str, Analyzer] = {a.id: a for a in analyzers}

    def _close_over(a: Analyzer) -> None:
        for dep_id in a.requires.upstream:
            if dep_id not in chosen:
                dep = by_id(dep_id)
                if dep is None:
                    raise ValueError(
                        f"analyzer {a.id!r} declares unknown upstream {dep_id!r}"
                    )
                chosen[dep_id] = dep
                _close_over(dep)

    for a in list(chosen.values()):
        _close_over(a)

    ordered: list[Analyzer] = []
    done: set[str] = set()
    visiting: set[str] = set()

    def _visit(a: Analyzer) -> None:
        if a.id in done:
            return
        if a.id in visiting:
            raise ValueError(f"analyzer dependency cycle through {a.id!r}")
        visiting.add(a.id)
        for dep_id in a.requires.upstream:
            _visit(chosen[dep_id])
        visiting.discard(a.id)
        done.add(a.id)
        ordered.append(a)

    for a in chosen.values():
        _visit(a)
    return ordered


def _meet_quality(own: Quality, upstream: Sequence[MetricResult]) -> Quality:
    """Micro-decision 17: the meet — AND on calibrated, min on confidence,
    union on flags. n/n_excluded/n_ambiguous stay the analyzer's own (they
    describe its sample). v1 meets over ALL consumed upstream results;
    scope-aware refinement arrives if an analyzer ever needs it."""
    calibrated = own.calibrated
    confidence = own.confidence
    flags = set(own.flags)
    for r in upstream:
        q = r.quality
        if q.calibrated is not None:
            calibrated = q.calibrated if calibrated is None else (calibrated and q.calibrated)
        if q.confidence is not None:
            confidence = q.confidence if confidence is None else min(confidence, q.confidence)
        flags.update(q.flags)
    return replace(
        own,
        calibrated=calibrated,
        confidence=confidence,
        flags=tuple(sorted(flags)),
    )


def run_analyses(
    session: Session,
    *,
    profile: KitProfile | None = None,
    analyzers: Sequence[Analyzer] | None = None,
) -> AnalysisReport:
    """Evaluate analyzers over one session. Pure: no I/O, no persistence.

    ``analyzers=None`` runs the full registry. An explicit subset is closed
    over its upstream dependencies automatically."""
    if analyzers is None:
        from edrum.engine.analyzers import ALL  # lazy: avoids import cycle

        analyzers = ALL

    ordered = _resolve(analyzers)
    inputs = build_inputs(session, profile)

    warnings = list(session.warnings)
    if _looks_undated(session.meta.start_iso):
        warnings.append(f"undated: start_iso {session.meta.start_iso!r} predates 2000 "
                        "(pre-settime cold boot?) — longitudinal ordering unreliable")

    results: list[MetricResult] = []
    by_analyzer: dict[str, tuple[MetricResult, ...]] = {}
    refused: dict[str, Refusal] = {}

    for a in ordered:
        req = a.requires
        refusal: Refusal | None = None
        refused_upstream = [d for d in req.upstream if d in refused]
        if refused_upstream:
            refusal = Refusal(
                a.id,
                "upstream_refused",
                f"via {', '.join(refused_upstream)}",
            )
        elif req.needs_grid and not session.grid_track:
            refusal = Refusal(a.id, "no_grid_segments", "nothing is gradeable — no grade span was declared")
        elif req.needs_profile and profile is None:
            refusal = Refusal(a.id, "no_profile", "no kit profile resolved for lane views")
        if refusal is not None:
            refused[a.id] = refusal
            continue

        upstream_map = {d: by_analyzer[d] for d in req.upstream}
        upstream_flat = [r for d in req.upstream for r in by_analyzer[d]]
        out = []
        for r in a.compute(inputs, upstream_map):
            assert r.analyzer_id == a.id and r.analyzer_version == a.version, (
                f"analyzer {a.id!r} emitted a result stamped {r.analyzer_id!r} "
                f"v{r.analyzer_version}"
            )
            out.append(replace(r, quality=_meet_quality(r.quality, upstream_flat)))
        by_analyzer[a.id] = tuple(out)
        results.extend(out)

    return AnalysisReport(
        session_id=session.meta.session_id,
        schema_version=session.meta.schema_version,
        analysis_version=ANALYSIS_VERSION,
        kit_profile_id=session.meta.kit_profile_id,
        profile_id=profile.profile_id if profile is not None else None,
        profile_version=profile.profile_version if profile is not None else None,
        calibrated=inputs.calibrated,
        calibration_offset_ms=inputs.calibration_offset_ms,
        warnings=tuple(warnings),
        results=tuple(results),
        refusals=tuple(refused.values()),
    )


# ---------------------------------------------------------------------------
# result → dict (pure; the CLI's --json and any future caller share it)
# ---------------------------------------------------------------------------

def _jsonable(value: object) -> object:
    if value is None or isinstance(value, (bool, int, float, str)):
        return value
    if isinstance(value, (list, tuple)):
        return [_jsonable(v) for v in value]
    if hasattr(value, "__dataclass_fields__"):
        return {
            name: _jsonable(getattr(value, name))
            for name in value.__dataclass_fields__
        }
    raise TypeError(f"unserializable result value: {type(value).__name__}")


def _scope_to_dict(scope: Scope) -> dict:
    out: dict = {}
    if scope.span is not None:
        out["span"] = {
            "kind": scope.span.kind,
            "start_t": scope.span.start_t,
            "end_t": scope.span.end_t,
        }
    for axis in ("lane", "articulation", "note", "bpm", "subdiv", "grid_source"):
        v = getattr(scope, axis)
        if v is not None:
            out[axis] = v
    return out


def report_to_dict(report: AnalysisReport) -> dict:
    """Pure conversion for JSON emission. The shape may evolve freely
    between engine versions (micro-decision 11) — it is a view, not a
    contract; golden-value tests pin the numbers, not this layout."""
    return {
        "session_id": report.session_id,
        "schema_version": report.schema_version,
        "analysis_version": report.analysis_version,
        "kit_profile_id": report.kit_profile_id,
        "profile_id": report.profile_id,
        "profile_version": report.profile_version,
        "calibrated": report.calibrated,
        "calibration_offset_ms": report.calibration_offset_ms,
        "warnings": list(report.warnings),
        "results": [
            {
                "analyzer_id": r.analyzer_id,
                "analyzer_version": r.analyzer_version,
                "scope": _scope_to_dict(r.scope),
                "unit": r.unit,
                "value": _jsonable(r.value),
                "quality": {
                    "n": r.quality.n,
                    "n_excluded": r.quality.n_excluded,
                    "n_ambiguous": r.quality.n_ambiguous,
                    "calibrated": r.quality.calibrated,
                    "confidence": r.quality.confidence,
                    "flags": list(r.quality.flags),
                },
            }
            for r in report.results
        ],
        "refusals": [
            {"analyzer_id": f.analyzer_id, "reason": f.reason, "detail": f.detail}
            for f in report.refusals
        ],
    }
