"""S2 — the append-only session log on disk (brain spec §3; capture spec §6).

Writer contract:
    * appends canonical lines only — never seeks, never rewrites line 1
    * flushes per line, so the file is valid up to the last complete line at
      every instant (crash-survivable by construction)
    * asserts non-decreasing ``t`` (producer-side bug trap; the load-bearing
      monotonicity invariant of spec §3)

Reader contract (crash recovery = truncate to the last complete line):
    * a torn trailing line (no LF, or unparseable final line) is dropped and
      reported in ``RecoveryInfo`` — the file on disk is NEVER modified
    * an unparseable line anywhere else is corruption, not crash damage,
      and raises ``LogCorruptionError``
    * a newer-major meta propagates ``SchemaVersionError`` (refusal, spec §3)
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

from edrum.engine.records import (
    MetaRecord,
    Record,
    RecordError,
    SchemaVersionError,
    SessionEndRecord,
    parse_line,
    to_line,
)


class LogError(Exception):
    """Misuse of the log API or an invalid log file."""


class InvalidLogError(LogError):
    """The file is not a session log (empty, or line 1 is not meta)."""


class LogCorruptionError(LogError):
    """Damage beyond the torn-tail crash model."""


def session_filename(start: datetime | str, session_id: str) -> str:
    """Approved naming (micro-decision 5): ``YYYYMMDDTHHMMSS_<id8>.jsonl``."""
    dt = datetime.fromisoformat(start) if isinstance(start, str) else start
    return f"{dt.strftime('%Y%m%dT%H%M%S')}_{session_id[:8]}.jsonl"


@dataclass(frozen=True)
class RecoveryInfo:
    truncated: bool
    dropped_bytes: int
    has_session_end: bool


@dataclass(frozen=True)
class LogRead:
    meta: MetaRecord
    records: list[Record]
    recovery: RecoveryInfo


class SessionWriter:
    """Append-only writer. The only component that ever writes a session file."""

    def __init__(self, path: str | Path, meta: MetaRecord, *, fsync_every: int | None = None):
        if not isinstance(meta, MetaRecord):
            raise LogError("line 1 must be a MetaRecord")
        self._path = Path(path)
        # "xb": refuse to clobber an existing session — files are never rewritten.
        self._f = open(self._path, "xb")
        self._fsync_every = fsync_every
        self._lines_since_sync = 0
        self._last_t = 0
        self._ended = False
        self._closed = False
        self._write(to_line(meta))

    def _write(self, line: bytes) -> None:
        self._f.write(line)
        self._f.flush()
        if self._fsync_every is not None:
            self._lines_since_sync += 1
            if self._lines_since_sync >= self._fsync_every:
                os.fsync(self._f.fileno())
                self._lines_since_sync = 0

    def append(self, record: Record) -> None:
        if self._closed:
            raise LogError("writer is closed")
        if isinstance(record, MetaRecord):
            raise LogError("meta appears exactly once, at line 1 (append-only log)")
        if self._ended:
            raise LogError("session already ended")
        t = getattr(record, "t", None)
        if t is not None:
            if t < self._last_t:
                raise LogError(
                    f"non-monotonic t: {t} after {self._last_t} "
                    "(t is monotonic session wall-time, spec §3)"
                )
            self._last_t = t
        self._write(to_line(record))
        if isinstance(record, SessionEndRecord):
            self._ended = True

    def close(self, end_t: int | None = None) -> None:
        """Clean close: writes ``session_end`` (unless one was already appended)."""
        if self._closed:
            return
        if not self._ended:
            self.append(SessionEndRecord(t=self._last_t if end_t is None else end_t))
        self._closed = True
        self._f.close()

    def abort(self) -> None:
        """Close the handle without a ``session_end`` — simulates/handles an
        unclean stop. The file remains a valid (recoverable) log."""
        if not self._closed:
            self._closed = True
            self._f.close()

    @property
    def path(self) -> Path:
        return self._path

    @property
    def last_t(self) -> int:
        return self._last_t


def read_log(path: str | Path) -> LogRead:
    """Read a session log, applying crash recovery (truncate to last complete line)."""
    data = Path(path).read_bytes()
    if not data:
        raise InvalidLogError(f"{path}: empty file")

    lines = data.split(b"\n")
    torn_tail = lines.pop()  # b"" when the file ends with LF
    dropped = len(torn_tail)
    truncated = dropped > 0

    parsed: list[Record] = []
    for i, line in enumerate(lines):
        try:
            parsed.append(parse_line(line))
        except RecordError as exc:
            # SchemaVersionError subclasses RecordError but is a refusal, not damage.
            if isinstance(exc, SchemaVersionError):
                raise
            if i == len(lines) - 1:
                # torn final line that happened to get its LF out — crash model
                dropped += len(line) + 1
                truncated = True
                break
            raise LogCorruptionError(f"{path}: line {i + 1}: {exc}") from exc

    if not parsed or not isinstance(parsed[0], MetaRecord):
        raise InvalidLogError(f"{path}: line 1 must be a meta record")
    if any(isinstance(r, MetaRecord) for r in parsed[1:]):
        raise LogCorruptionError(f"{path}: meta record appears after line 1")

    meta = parsed[0]
    records = parsed[1:]
    has_end = any(isinstance(r, SessionEndRecord) for r in records)
    return LogRead(
        meta=meta,
        records=records,
        recovery=RecoveryInfo(truncated=truncated, dropped_bytes=dropped, has_session_end=has_end),
    )


def load_session(path: str | Path):
    """Convenience: read a log and fold it (io may import engine, never reverse)."""
    from edrum.engine.session import reduce_session

    lr = read_log(path)
    return reduce_session(lr.meta, lr.records), lr.recovery
