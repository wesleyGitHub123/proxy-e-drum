"""S3 — Session reduction: fold the record stream into the §3 Session object.

The §3 Session is the *parsed reduction* of the append-only log, not the file
layout: declaration lines fold into ``grid_track`` / ``enrollment_spans`` /
``bookmarks`` on read. Grading is NOT computed here and is never stored
anywhere (spec §1A invariant 2) — a span is gradeable iff a grid segment
covers it, and that geometry is derived downstream, on demand.

Fold semantics (spec §5 declaration mechanics):
    * ``grid_start`` while a grid span is open = the param-change rule: the
      open segment closes at the new start and a new one opens there
    * unmatched ``grid_end`` / ``enroll_end`` → warning, ignored
    * dangling open spans auto-close at the last event within them
      (degenerating to ``start_t`` when the span holds no events)
    * unknown record types are ignored (tallied in warnings)
    * foreign-producer anomalies (non-monotonic t, records after session_end)
      warn — the fold never fails on a structurally valid log
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Iterable

from edrum.engine.records import (
    BookmarkRecord,
    CtrlRecord,
    EnrollEndRecord,
    EnrollStartRecord,
    EventRecord,
    GridEndRecord,
    GridStartRecord,
    MetaRecord,
    Record,
    SessionEndRecord,
    UnknownRecord,
)


@dataclass(frozen=True)
class GridSegment:
    """One declared graded span's exact grid (spec §3 grid_track entry)."""

    start_t: int
    end_t: int
    bpm: int | float
    subdiv: int
    downbeat_t: int


@dataclass(frozen=True)
class EnrollmentSpan:
    """A declared groove demonstration with its click snapshot (spec §3)."""

    start_t: int
    end_t: int
    profile_ref: str
    bpm: int | float
    subdiv: int
    downbeat_t: int


@dataclass
class Session:
    """The parsed reduction (spec §3): one performance track + sparse grid track."""

    meta: MetaRecord
    events: list[EventRecord] = field(default_factory=list)
    ctrl: list[CtrlRecord] = field(default_factory=list)
    grid_track: list[GridSegment] = field(default_factory=list)
    enrollment_spans: list[EnrollmentSpan] = field(default_factory=list)
    bookmarks: list[int] = field(default_factory=list)
    end_t: int | None = None
    warnings: list[str] = field(default_factory=list)


def _last_event_t_in(events: list[EventRecord], start_t: int) -> int:
    """Auto-close point: t of the last event within the span (spec §5)."""
    for ev in reversed(events):
        if ev.t >= start_t:
            return ev.t
    return start_t


def reduce_session(meta: MetaRecord, records: Iterable[Record]) -> Session:
    """Pure fold: (meta, record stream) → Session. No I/O, no side effects."""
    s = Session(meta=meta)
    open_grid: GridStartRecord | None = None
    open_enroll: EnrollStartRecord | None = None
    unknown_count = 0
    last_t: int | None = None
    warned_monotonic = False
    ended = False

    for rec in records:
        if isinstance(rec, MetaRecord):
            s.warnings.append("meta record after line 1 ignored")
            continue

        t = getattr(rec, "t", None)
        if t is not None:
            if last_t is not None and t < last_t and not warned_monotonic:
                s.warnings.append(
                    f"non-monotonic t ({t} after {last_t}) — foreign producer bug?"
                )
                warned_monotonic = True
            if last_t is None or t > last_t:
                last_t = t
        if ended and not isinstance(rec, UnknownRecord):
            s.warnings.append(f"record after session_end at t={t}")
            ended = False  # warn once per stretch, keep folding

        if isinstance(rec, EventRecord):
            s.events.append(rec)
        elif isinstance(rec, CtrlRecord):
            s.ctrl.append(rec)
        elif isinstance(rec, BookmarkRecord):
            s.bookmarks.append(rec.t)
        elif isinstance(rec, GridStartRecord):
            if open_grid is not None:
                # param-change rule (§5): close at the change point, open anew
                s.grid_track.append(
                    GridSegment(
                        start_t=open_grid.t,
                        end_t=rec.t,
                        bpm=open_grid.bpm,
                        subdiv=open_grid.subdiv,
                        downbeat_t=open_grid.downbeat_t,
                    )
                )
            open_grid = rec
        elif isinstance(rec, GridEndRecord):
            if open_grid is None:
                s.warnings.append(f"unmatched grid_end at t={rec.t} ignored")
            else:
                end_t = rec.t
                if end_t < open_grid.t:
                    s.warnings.append(
                        f"grid_end t={end_t} before its grid_start t={open_grid.t}; clamped"
                    )
                    end_t = open_grid.t
                s.grid_track.append(
                    GridSegment(
                        start_t=open_grid.t,
                        end_t=end_t,
                        bpm=open_grid.bpm,
                        subdiv=open_grid.subdiv,
                        downbeat_t=open_grid.downbeat_t,
                    )
                )
                open_grid = None
        elif isinstance(rec, EnrollStartRecord):
            if open_enroll is not None:
                s.warnings.append(
                    f"enroll_start at t={rec.t} while one open; previous closed here"
                )
                s.enrollment_spans.append(
                    EnrollmentSpan(
                        start_t=open_enroll.t,
                        end_t=rec.t,
                        profile_ref=open_enroll.profile_ref,
                        bpm=open_enroll.bpm,
                        subdiv=open_enroll.subdiv,
                        downbeat_t=open_enroll.downbeat_t,
                    )
                )
            open_enroll = rec
        elif isinstance(rec, EnrollEndRecord):
            if open_enroll is None:
                s.warnings.append(f"unmatched enroll_end at t={rec.t} ignored")
            else:
                end_t = max(rec.t, open_enroll.t)
                s.enrollment_spans.append(
                    EnrollmentSpan(
                        start_t=open_enroll.t,
                        end_t=end_t,
                        profile_ref=open_enroll.profile_ref,
                        bpm=open_enroll.bpm,
                        subdiv=open_enroll.subdiv,
                        downbeat_t=open_enroll.downbeat_t,
                    )
                )
                open_enroll = None
        elif isinstance(rec, SessionEndRecord):
            s.end_t = rec.t
            ended = True
        elif isinstance(rec, UnknownRecord):
            unknown_count += 1
        # (no else: the Record union is closed above)

    # auto-close dangling spans (spec §5: "a forgotten end never produces an
    # unbounded segment") at the last event in them
    if open_grid is not None:
        end_t = _last_event_t_in(s.events, open_grid.t)
        s.grid_track.append(
            GridSegment(
                start_t=open_grid.t,
                end_t=end_t,
                bpm=open_grid.bpm,
                subdiv=open_grid.subdiv,
                downbeat_t=open_grid.downbeat_t,
            )
        )
        s.warnings.append(f"open grid span auto-closed at t={end_t}")
    if open_enroll is not None:
        end_t = _last_event_t_in(s.events, open_enroll.t)
        s.enrollment_spans.append(
            EnrollmentSpan(
                start_t=open_enroll.t,
                end_t=end_t,
                profile_ref=open_enroll.profile_ref,
                bpm=open_enroll.bpm,
                subdiv=open_enroll.subdiv,
                downbeat_t=open_enroll.downbeat_t,
            )
        )
        s.warnings.append(f"open enrollment span auto-closed at t={end_t}")

    if unknown_count:
        s.warnings.append(f"ignored {unknown_count} unknown-type line(s)")
    return s
