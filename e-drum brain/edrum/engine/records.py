"""S1 — Canonical record schema & serialization.

This module IS the session-file contract (brain spec §3, phase0-plan S1).
A session log is an append-only sequence of typed JSON lines: line 1 is a
``meta`` record; every subsequent line is one timestamped occurrence.

Canonical serialization (phase0-plan micro-decision 6, FROZEN):
    * UTF-8, no BOM, LF line endings
    * compact separators ("," and ":")
    * deterministic per-type key order (defined by the ``_*_obj`` functions below)
    * integers wherever the schema says integer (``t`` ms, ``note``/``velocity`` 0-127)
    * inside a ctrl ``msg``: ``type`` first, remaining keys sorted alphabetically

Reader policy (brain spec §3, schema evolution):
    * unknown fields on known record types are IGNORED (additive minor evolution)
    * unknown record TYPES are preserved byte-faithfully as ``UnknownRecord`` and
      ignored semantically (reserves e.g. ``pause``/``resume``)
    * a ``meta`` with ``schema_version`` newer than ``SCHEMA_VERSION`` is REFUSED
      (single-integer major version — micro-decision 4)
    * files are never rewritten in place

Validation lives in ``__post_init__`` so that *any* constructed record is valid
by construction; parsers only extract and construct.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Union

#: Single integer = MAJOR compatibility only (micro-decision 4). Additive
#: (minor) evolution is self-describing under the ignore-unknown-fields policy.
SCHEMA_VERSION = 1

#: System-realtime message types dropped at capture (micro-decision 2).
#: Transport keepalive, not performance information. All channel-scoped
#: messages and sysex are preserved.
REALTIME_FILTER = frozenset(
    {"clock", "start", "continue", "stop", "active_sensing", "reset"}
)


class RecordError(ValueError):
    """A line or record violates the schema."""


class SchemaVersionError(RecordError):
    """File requires a newer major schema than this reader supports."""


# ---------------------------------------------------------------------------
# validation helpers
# ---------------------------------------------------------------------------

def _is_int(v: object) -> bool:
    # bool is a subclass of int; a JSON `true` must never pass as an integer.
    return isinstance(v, int) and not isinstance(v, bool)


def _check_t(t: object) -> None:
    if not _is_int(t) or t < 0:
        raise RecordError(f"t must be a non-negative integer (ms), got {t!r}")


def _check_7bit(name: str, v: object) -> None:
    if not _is_int(v) or not (0 <= v <= 127):
        raise RecordError(f"{name} must be an integer 0-127, got {v!r}")


def _check_str(name: str, v: object) -> None:
    if not isinstance(v, str) or not v:
        raise RecordError(f"{name} must be a non-empty string, got {v!r}")


def _check_number(name: str, v: object) -> None:
    if _is_int(v):
        return
    if isinstance(v, float) and v == v and v not in (float("inf"), float("-inf")):
        return
    raise RecordError(f"{name} must be a finite number, got {v!r}")


# ---------------------------------------------------------------------------
# record types
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class MetaRecord:
    """Line 1 of every session log (brain spec §3 meta)."""

    schema_version: int
    session_id: str
    start_iso: str
    kit_profile_id: str | None  # reassignable pointer, never a welded fact (§4.2)
    user_id: str
    calibration_offset_ms: int | float | None  # per-session; null = uncalibrated (§6)

    def __post_init__(self) -> None:
        if not _is_int(self.schema_version) or self.schema_version < 1:
            raise RecordError(
                f"schema_version must be a positive integer, got {self.schema_version!r}"
            )
        _check_str("session_id", self.session_id)
        _check_str("start_iso", self.start_iso)
        if self.kit_profile_id is not None:
            _check_str("kit_profile_id", self.kit_profile_id)
        _check_str("user_id", self.user_id)
        if self.calibration_offset_ms is not None:
            _check_number("calibration_offset_ms", self.calibration_offset_ms)


@dataclass(frozen=True)
class EventRecord:
    """One performance-track hit: the (t, note, velocity) atom (§3)."""

    t: int
    note: int
    velocity: int
    channel: int | None = None

    def __post_init__(self) -> None:
        _check_t(self.t)
        _check_7bit("note", self.note)
        _check_7bit("velocity", self.velocity)
        if self.channel is not None and (
            not _is_int(self.channel) or not (0 <= self.channel <= 15)
        ):
            raise RecordError(f"channel must be an integer 0-15, got {self.channel!r}")


@dataclass(frozen=True)
class CtrlRecord:
    """Raw non-event channel message, preserved losslessly (§3 ctrl atom).

    ``msg`` is a structured mido-style dict (micro-decision 1) with the mido
    ``time`` field stripped — our ``t`` is authoritative. Includes note-off /
    note-on-velocity-0 (micro-decision 3): preserved-but-invisible, never a
    piano-roll lane.
    """

    t: int
    msg: dict

    def __post_init__(self) -> None:
        _check_t(self.t)
        if not isinstance(self.msg, dict):
            raise RecordError(f"ctrl msg must be an object, got {type(self.msg).__name__}")
        mtype = self.msg.get("type")
        if not isinstance(mtype, str) or not mtype:
            raise RecordError("ctrl msg must carry a non-empty string 'type'")
        if "time" in self.msg:
            raise RecordError("ctrl msg must not carry mido 'time' (t is authoritative)")
        for k, v in self.msg.items():
            if not isinstance(k, str):
                raise RecordError(f"ctrl msg keys must be strings, got {k!r}")
            if _is_int(v) or isinstance(v, str):
                continue
            if isinstance(v, list) and all(_is_int(x) for x in v):
                continue  # sysex data bytes
            raise RecordError(
                f"ctrl msg value for {k!r} must be int, str, or list of ints, got {v!r}"
            )


@dataclass(frozen=True)
class GridStartRecord:
    """Grade-span declaration: snapshots the running click (§5). Writes the grid."""

    t: int
    bpm: int | float
    subdiv: int
    downbeat_t: int

    def __post_init__(self) -> None:
        _check_t(self.t)
        _check_number("bpm", self.bpm)
        if self.bpm <= 0:
            raise RecordError(f"bpm must be > 0, got {self.bpm!r}")
        if not _is_int(self.subdiv) or self.subdiv < 1:
            raise RecordError(f"subdiv must be a positive integer, got {self.subdiv!r}")
        _check_t(self.downbeat_t)


@dataclass(frozen=True)
class GridEndRecord:
    t: int

    def __post_init__(self) -> None:
        _check_t(self.t)


@dataclass(frozen=True)
class BookmarkRecord:
    t: int

    def __post_init__(self) -> None:
        _check_t(self.t)


@dataclass(frozen=True)
class EnrollStartRecord:
    """Enrollment declaration: snapshots the click identically to a grade span (§3/3f)."""

    t: int
    profile_ref: str  # stable, user-assigned groove identity (§3)
    bpm: int | float
    subdiv: int
    downbeat_t: int

    def __post_init__(self) -> None:
        _check_t(self.t)
        _check_str("profile_ref", self.profile_ref)
        _check_number("bpm", self.bpm)
        if self.bpm <= 0:
            raise RecordError(f"bpm must be > 0, got {self.bpm!r}")
        if not _is_int(self.subdiv) or self.subdiv < 1:
            raise RecordError(f"subdiv must be a positive integer, got {self.subdiv!r}")
        _check_t(self.downbeat_t)


@dataclass(frozen=True)
class EnrollEndRecord:
    t: int

    def __post_init__(self) -> None:
        _check_t(self.t)


@dataclass(frozen=True)
class SessionEndRecord:
    t: int

    def __post_init__(self) -> None:
        _check_t(self.t)


@dataclass(frozen=True)
class UnknownRecord:
    """A line whose ``type`` this reader does not know.

    Preserved byte-faithfully (``raw_line``) so re-emission is lossless, and
    ignored semantically by the session fold. This is how new line types stay
    additive (reader policy, §3).
    """

    type: str
    t: int | None
    raw: dict
    raw_line: bytes  # original line bytes, LF-terminated

    def __post_init__(self) -> None:
        if not isinstance(self.raw_line, bytes) or not self.raw_line.endswith(b"\n"):
            raise RecordError("UnknownRecord.raw_line must be LF-terminated bytes")


Record = Union[
    MetaRecord,
    EventRecord,
    CtrlRecord,
    GridStartRecord,
    GridEndRecord,
    BookmarkRecord,
    EnrollStartRecord,
    EnrollEndRecord,
    SessionEndRecord,
    UnknownRecord,
]


# ---------------------------------------------------------------------------
# canonical serialization (micro-decision 6 — FROZEN key orders)
# ---------------------------------------------------------------------------

def _canonical_msg(msg: dict) -> dict:
    """ctrl msg canonical order: 'type' first, remaining keys sorted."""
    return {"type": msg["type"], **{k: msg[k] for k in sorted(msg) if k != "type"}}


def _meta_obj(r: MetaRecord) -> dict:
    return {
        "type": "meta",
        "schema_version": r.schema_version,
        "session_id": r.session_id,
        "start_iso": r.start_iso,
        "kit_profile_id": r.kit_profile_id,
        "user_id": r.user_id,
        "calibration_offset_ms": r.calibration_offset_ms,
    }


def _event_obj(r: EventRecord) -> dict:
    obj = {"type": "event", "t": r.t, "note": r.note, "velocity": r.velocity}
    if r.channel is not None:
        obj["channel"] = r.channel
    return obj


def _ctrl_obj(r: CtrlRecord) -> dict:
    return {"type": "ctrl", "t": r.t, "msg": _canonical_msg(r.msg)}


def _grid_start_obj(r: GridStartRecord) -> dict:
    return {
        "type": "grid_start",
        "t": r.t,
        "bpm": r.bpm,
        "subdiv": r.subdiv,
        "downbeat_t": r.downbeat_t,
    }


def _grid_end_obj(r: GridEndRecord) -> dict:
    return {"type": "grid_end", "t": r.t}


def _bookmark_obj(r: BookmarkRecord) -> dict:
    return {"type": "bookmark", "t": r.t}


def _enroll_start_obj(r: EnrollStartRecord) -> dict:
    return {
        "type": "enroll_start",
        "t": r.t,
        "profile_ref": r.profile_ref,
        "bpm": r.bpm,
        "subdiv": r.subdiv,
        "downbeat_t": r.downbeat_t,
    }


def _enroll_end_obj(r: EnrollEndRecord) -> dict:
    return {"type": "enroll_end", "t": r.t}


def _session_end_obj(r: SessionEndRecord) -> dict:
    return {"type": "session_end", "t": r.t}


_TO_OBJ = {
    MetaRecord: _meta_obj,
    EventRecord: _event_obj,
    CtrlRecord: _ctrl_obj,
    GridStartRecord: _grid_start_obj,
    GridEndRecord: _grid_end_obj,
    BookmarkRecord: _bookmark_obj,
    EnrollStartRecord: _enroll_start_obj,
    EnrollEndRecord: _enroll_end_obj,
    SessionEndRecord: _session_end_obj,
}


def to_line(record: Record) -> bytes:
    """Serialize a record to its one canonical LF-terminated line."""
    if isinstance(record, UnknownRecord):
        return record.raw_line
    obj = _TO_OBJ[type(record)](record)
    return (
        json.dumps(obj, ensure_ascii=False, separators=(",", ":"), allow_nan=False)
        .encode("utf-8")
        + b"\n"
    )


# ---------------------------------------------------------------------------
# parsing (reader policy: ignore unknown fields; refuse newer major;
#          unknown types -> UnknownRecord)
# ---------------------------------------------------------------------------

def _parse_meta(obj: dict) -> MetaRecord:
    version = obj.get("schema_version")
    if not _is_int(version):
        raise RecordError(f"meta schema_version must be an integer, got {version!r}")
    if version > SCHEMA_VERSION:
        raise SchemaVersionError(
            f"file schema_version {version} is newer than supported {SCHEMA_VERSION}; refusing"
        )
    return MetaRecord(
        schema_version=version,
        session_id=obj.get("session_id"),
        start_iso=obj.get("start_iso"),
        kit_profile_id=obj.get("kit_profile_id"),
        user_id=obj.get("user_id"),
        calibration_offset_ms=obj.get("calibration_offset_ms"),
    )


def _parse_event(obj: dict) -> EventRecord:
    return EventRecord(
        t=obj.get("t"),
        note=obj.get("note"),
        velocity=obj.get("velocity"),
        channel=obj.get("channel"),
    )


def _parse_ctrl(obj: dict) -> CtrlRecord:
    return CtrlRecord(t=obj.get("t"), msg=obj.get("msg"))


def _parse_grid_start(obj: dict) -> GridStartRecord:
    return GridStartRecord(
        t=obj.get("t"),
        bpm=obj.get("bpm"),
        subdiv=obj.get("subdiv"),
        downbeat_t=obj.get("downbeat_t"),
    )


def _parse_grid_end(obj: dict) -> GridEndRecord:
    return GridEndRecord(t=obj.get("t"))


def _parse_bookmark(obj: dict) -> BookmarkRecord:
    return BookmarkRecord(t=obj.get("t"))


def _parse_enroll_start(obj: dict) -> EnrollStartRecord:
    return EnrollStartRecord(
        t=obj.get("t"),
        profile_ref=obj.get("profile_ref"),
        bpm=obj.get("bpm"),
        subdiv=obj.get("subdiv"),
        downbeat_t=obj.get("downbeat_t"),
    )


def _parse_enroll_end(obj: dict) -> EnrollEndRecord:
    return EnrollEndRecord(t=obj.get("t"))


def _parse_session_end(obj: dict) -> SessionEndRecord:
    return SessionEndRecord(t=obj.get("t"))


_PARSERS = {
    "meta": _parse_meta,
    "event": _parse_event,
    "ctrl": _parse_ctrl,
    "grid_start": _parse_grid_start,
    "grid_end": _parse_grid_end,
    "bookmark": _parse_bookmark,
    "enroll_start": _parse_enroll_start,
    "enroll_end": _parse_enroll_end,
    "session_end": _parse_session_end,
}


def parse_line(line: bytes | str) -> Record:
    """Parse one log line into a typed record.

    Raises ``RecordError`` on malformed JSON / schema violations and
    ``SchemaVersionError`` on a newer-major meta. Unknown record types come
    back as ``UnknownRecord`` (never an error).
    """
    raw = line.encode("utf-8") if isinstance(line, str) else bytes(line)
    stripped = raw.strip(b"\r\n ")
    if not stripped:
        raise RecordError("empty line")
    try:
        obj = json.loads(stripped)
    except json.JSONDecodeError as exc:
        raise RecordError(f"invalid JSON: {exc}") from exc
    if not isinstance(obj, dict):
        raise RecordError(f"line must be a JSON object, got {type(obj).__name__}")
    rtype = obj.get("type")
    if not isinstance(rtype, str) or not rtype:
        raise RecordError("line missing string 'type' field")
    parser = _PARSERS.get(rtype)
    if parser is None:
        t = obj.get("t")
        return UnknownRecord(
            type=rtype,
            t=t if _is_int(t) and t >= 0 else None,
            raw=obj,
            raw_line=stripped + b"\n",
        )
    return parser(obj)
