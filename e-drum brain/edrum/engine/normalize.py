"""S6 — normalization: raw event × kit profile → derived view (spec §4.2).

A derived VIEW, never stored (invariant 3): the raw note is always preserved
losslessly in the session file; this function re-derives instrument and
articulation on read, which is what makes profile fixes retroactive for free
(the SSD property).

Total by design: an unknown note lands on the visible UNMAPPED lane. This
function must never raise on any valid EventRecord.
"""

from __future__ import annotations

from dataclasses import dataclass

from edrum.engine.records import EventRecord
from edrum.engine.profiles import KitProfile
from edrum.engine.vocab import LIMB_CLASS, Instrument, LimbClass

_UNMAPPED_LIMB = LIMB_CLASS[Instrument.UNMAPPED]


@dataclass(frozen=True)
class NormalizedView:
    instrument: Instrument
    articulation: str | None
    limb: LimbClass


def normalize_event(event: EventRecord, profile: KitProfile) -> NormalizedView:
    """Total mapping: every event gets a lane; unknown notes → UNMAPPED."""
    entry = profile.note_map.get(event.note)
    if entry is None:
        return NormalizedView(Instrument.UNMAPPED, None, _UNMAPPED_LIMB)
    instrument, articulation = entry
    return NormalizedView(instrument, articulation, LIMB_CLASS[instrument])
