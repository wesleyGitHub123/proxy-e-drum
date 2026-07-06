"""S7 — the capture host pipeline (capture spec §4A): producer #1.

The receiver loop: drain records from a Source into the append-only writer.
Deliberately mirrors the firmware architecture (capture spec §2) — the capture
edge (stamping, in the source's callback thread) is isolated from storage by a
queue, so a slow flush never stalls capture and never smears a timestamp.

Zero analysis, zero normalization, ever (the two-machine split, spec §1).
"""

from __future__ import annotations

from typing import Callable

from edrum.engine.records import Record
from edrum.io.logfile import SessionWriter
from edrum.io.sources import Source


def run_capture(
    source: Source,
    writer: SessionWriter,
    *,
    on_record: Callable[[Record], None] | None = None,
) -> int:
    """Drain ``source.events()`` into ``writer``. Returns the record count.

    Stopping is the caller's concern: a live source stops yielding after its
    ``stop()``; scripted/file sources stop when exhausted. The caller owns
    ``writer.close(end_t)`` / ``source.close()`` (see cli.record).
    """
    count = 0
    for record in source.events():
        writer.append(record)
        count += 1
        if on_record is not None:
            on_record(record)
    return count
