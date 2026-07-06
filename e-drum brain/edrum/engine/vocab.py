"""S6 — the canonical vocabulary (brain spec §4.1, decision 4a: RESOLVED).

Two axes, never tangled:
    * Instrument = the piano-roll lane. A small, FROZEN, cross-kit set anchored
      to standard drum notation. Splitting an instrument later invalidates
      stored data; that is why the set is frozen and small.
    * Articulation = an optional, ADDITIVE attribute — populated where the kit
      sends it, None where it doesn't. Extending it is always safe.

The §7 hand-identity wall is enforced here at the type level: limb classes are
``foot`` and ``hands_unresolved`` only. There is deliberately no LEFT_HAND /
RIGHT_HAND anywhere — that information is not in pad-indexed MIDI, and the
engine refuses to fake it.
"""

from __future__ import annotations

from enum import Enum
from types import MappingProxyType


class Instrument(str, Enum):
    """The frozen lane set (~standard kit chart). DO NOT EXTEND — see spec §4.1.

    ``UNMAPPED`` is the catch-all: an unknown note lands on a visible lane
    instead of vanishing or crashing. 4+-tom kits gain an *additive* TOM_4
    later, never a resplit.
    """

    KICK = "KICK"
    SNARE = "SNARE"
    HIHAT = "HIHAT"
    HH_PEDAL = "HH_PEDAL"  # a FOOT event — its own instrument, not a HH articulation
    TOM_HIGH = "TOM_HIGH"
    TOM_MID = "TOM_MID"
    TOM_LOW = "TOM_LOW"
    RIDE = "RIDE"
    CRASH = "CRASH"  # never merges with RIDE — different musical function
    UNMAPPED = "UNMAPPED"


#: Valid articulation values per instrument (additive, null-where-absent).
#: Ghost notes are velocity, not articulation (spec §4.1).
ARTICULATIONS = MappingProxyType(
    {
        Instrument.KICK: frozenset(),
        Instrument.SNARE: frozenset({"head", "rim", "crossstick", "rimshot"}),
        Instrument.HIHAT: frozenset({"closed", "open", "half"}),
        Instrument.HH_PEDAL: frozenset(),
        Instrument.TOM_HIGH: frozenset({"head", "rim"}),
        Instrument.TOM_MID: frozenset({"head", "rim"}),
        Instrument.TOM_LOW: frozenset({"head", "rim"}),
        Instrument.RIDE: frozenset({"bow", "bell", "edge"}),
        Instrument.CRASH: frozenset({"bow", "edge"}),
        Instrument.UNMAPPED: frozenset(),
    }
)


class LimbClass(str, Enum):
    """Honest about the wall (spec §4.1/§7): feet are identifiable (separate
    pads); the two hands stay pooled because the data pools them."""

    FOOT = "foot"
    HANDS_UNRESOLVED = "hands_unresolved"


LIMB_CLASS = MappingProxyType(
    {
        inst: (
            LimbClass.FOOT
            if inst in (Instrument.KICK, Instrument.HH_PEDAL)
            else LimbClass.HANDS_UNRESOLVED
        )
        for inst in Instrument
    }
)
