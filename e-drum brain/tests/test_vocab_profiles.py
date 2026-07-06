"""S6 tests: frozen vocabulary, TD-02K profile, total normalization."""

from __future__ import annotations

from pathlib import Path

import pytest

from edrum.engine.normalize import NormalizedView, normalize_event
from edrum.engine.profiles import (
    Capabilities,
    ControllerSpec,
    ProfileError,
    load_profile,
    parse_profile,
)
from edrum.engine.records import EventRecord
from edrum.engine.vocab import ARTICULATIONS, LIMB_CLASS, Instrument, LimbClass

PROFILES_DIR = Path(__file__).parents[1] / "profiles"


# ---------------------------------------------------------------------------
# vocabulary: frozen means frozen
# ---------------------------------------------------------------------------

def test_instrument_set_is_exactly_the_frozen_ten():
    """Guard test (spec 4a): accidental lane growth must be a test failure."""
    assert {i.value for i in Instrument} == {
        "KICK",
        "SNARE",
        "HIHAT",
        "HH_PEDAL",
        "TOM_HIGH",
        "TOM_MID",
        "TOM_LOW",
        "RIDE",
        "CRASH",
        "UNMAPPED",
    }


def test_articulation_sets_match_spec_4_1():
    assert ARTICULATIONS[Instrument.SNARE] == {"head", "rim", "crossstick", "rimshot"}
    assert ARTICULATIONS[Instrument.HIHAT] == {"closed", "open", "half"}
    assert ARTICULATIONS[Instrument.RIDE] == {"bow", "bell", "edge"}
    assert ARTICULATIONS[Instrument.CRASH] == {"bow", "edge"}
    assert ARTICULATIONS[Instrument.TOM_MID] == {"head", "rim"}
    assert ARTICULATIONS[Instrument.KICK] == frozenset()


def test_hand_identity_wall_is_enforced_at_type_level():
    """Spec §7: no LEFT/RIGHT hand anywhere; feet identifiable, hands pooled."""
    assert {lc.value for lc in LimbClass} == {"foot", "hands_unresolved"}
    assert LIMB_CLASS[Instrument.KICK] is LimbClass.FOOT
    assert LIMB_CLASS[Instrument.HH_PEDAL] is LimbClass.FOOT
    assert LIMB_CLASS[Instrument.SNARE] is LimbClass.HANDS_UNRESOLVED
    assert LIMB_CLASS[Instrument.CRASH] is LimbClass.HANDS_UNRESOLVED


# ---------------------------------------------------------------------------
# TD-02K profile (approved chart data)
# ---------------------------------------------------------------------------

def test_td02k_loads_and_matches_verified_chart():
    p = load_profile(PROFILES_DIR / "td02k.json")
    assert p.profile_id == "td02k"
    assert p.profile_version == 1
    expected = {
        36: (Instrument.KICK, None),
        38: (Instrument.SNARE, "head"),
        40: (Instrument.SNARE, "rim"),
        37: (Instrument.SNARE, "crossstick"),
        48: (Instrument.TOM_HIGH, "head"),
        45: (Instrument.TOM_MID, "head"),
        43: (Instrument.TOM_LOW, "head"),
        46: (Instrument.HIHAT, "open"),
        26: (Instrument.HIHAT, "open"),
        42: (Instrument.HIHAT, "closed"),
        22: (Instrument.HIHAT, "closed"),
        44: (Instrument.HH_PEDAL, None),
        49: (Instrument.CRASH, "bow"),
        55: (Instrument.CRASH, "edge"),
        51: (Instrument.RIDE, "bow"),
        59: (Instrument.RIDE, "edge"),
        53: (Instrument.RIDE, "bell"),
    }
    assert dict(p.note_map) == expected
    assert p.capabilities == Capabilities(
        has_hh_pedal=True, has_positional_sensing=False, velocity_curve_id=None
    )
    assert p.hh_pedal_cc == ControllerSpec(controller=4, min=0, max=90)


# ---------------------------------------------------------------------------
# profile validation
# ---------------------------------------------------------------------------

def _minimal_profile(**overrides):
    data = {
        "profile_id": "test",
        "note_map": {"36": {"instrument": "KICK"}},
    }
    data.update(overrides)
    return data


def test_parse_profile_rejects_unknown_instrument():
    with pytest.raises(ProfileError, match="frozen instrument set"):
        parse_profile(_minimal_profile(note_map={"36": {"instrument": "COWBELL"}}))


def test_parse_profile_rejects_invalid_articulation():
    with pytest.raises(ProfileError, match="articulation"):
        parse_profile(_minimal_profile(note_map={"38": {"instrument": "SNARE", "articulation": "bell"}}))


def test_parse_profile_rejects_out_of_range_note():
    with pytest.raises(ProfileError, match="range"):
        parse_profile(_minimal_profile(note_map={"128": {"instrument": "KICK"}}))


def test_parse_profile_defaults():
    p = parse_profile(_minimal_profile())
    assert p.name == "test"
    assert p.profile_version == 1
    assert p.capabilities == Capabilities()
    assert p.hh_pedal_cc is None


# ---------------------------------------------------------------------------
# normalization: total, derived, re-runnable
# ---------------------------------------------------------------------------

def test_normalize_is_total_unknown_note_lands_on_unmapped():
    p = load_profile(PROFILES_DIR / "td02k.json")
    view = normalize_event(EventRecord(t=1, note=99, velocity=80), p)
    assert view == NormalizedView(Instrument.UNMAPPED, None, LimbClass.HANDS_UNRESOLVED)


def test_normalize_derives_lane_articulation_limb():
    p = load_profile(PROFILES_DIR / "td02k.json")
    view = normalize_event(EventRecord(t=1, note=44, velocity=60), p)
    assert view == NormalizedView(Instrument.HH_PEDAL, None, LimbClass.FOOT)
    view = normalize_event(EventRecord(t=1, note=53, velocity=90), p)
    assert view == NormalizedView(Instrument.RIDE, "bell", LimbClass.HANDS_UNRESOLVED)


def test_profile_swap_renormalizes_same_raw_event():
    """The SSD property (spec §4.2): same raw MIDI, different profile, different view."""
    event = EventRecord(t=1, note=50, velocity=80)
    profile_a = parse_profile(
        {"profile_id": "a", "note_map": {"50": {"instrument": "TOM_HIGH", "articulation": "head"}}}
    )
    profile_b = parse_profile(
        {"profile_id": "b", "note_map": {"50": {"instrument": "RIDE", "articulation": "bow"}}}
    )
    assert normalize_event(event, profile_a).instrument is Instrument.TOM_HIGH
    assert normalize_event(event, profile_b).instrument is Instrument.RIDE
    # and the raw event was never touched
    assert event.note == 50


def test_ghost_notes_are_velocity_not_articulation():
    """Spec §4.1: a ghost is a low-velocity SNARE head, never a token."""
    p = load_profile(PROFILES_DIR / "td02k.json")
    ghost = normalize_event(EventRecord(t=1, note=38, velocity=12), p)
    accent = normalize_event(EventRecord(t=1, note=38, velocity=120), p)
    assert ghost == accent  # identical view; the difference lives in velocity
