"""S6 — kit profiles (brain spec §4.2, decision 4b: RESOLVED).

A kit profile is DATA, not code: ``{note → (instrument, articulation)}`` plus
capability flags. Normalization is brain-side and re-runnable — a fixed or
improved profile retroactively re-normalizes all history for free, which is
the entire "works for any drummer" mechanism. A new kit is a new JSON file,
never a firmware fork.

``profile_version`` makes a re-run attributable (spec §4.2 registry
versioning, Tier 2 — the field is additive metadata; bumps never rewrite
session files).

Engine purity note: ``parse_profile`` is pure; ``load_profile`` takes an
explicit path (no baked-in file locations — the caller owns path resolution).
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from types import MappingProxyType
from typing import Mapping

from edrum.engine.vocab import ARTICULATIONS, Instrument


class ProfileError(ValueError):
    """A profile file violates the profile schema or the frozen vocabulary."""


@dataclass(frozen=True)
class ControllerSpec:
    """A kit-specific continuous controller (e.g. TD-02 hi-hat pedal CC4 0-90).

    Phase 0 stores this so the future openness derivation (HIHAT 'half' from
    pedal-position ctrl records, spec §4.1) has its parameters; nothing is
    derived from it yet.
    """

    controller: int
    min: int
    max: int


@dataclass(frozen=True)
class Capabilities:
    has_hh_pedal: bool = False
    has_positional_sensing: bool = False
    velocity_curve_id: str | None = None


@dataclass(frozen=True)
class KitProfile:
    profile_id: str
    name: str
    profile_version: int
    note_map: Mapping[int, tuple[Instrument, str | None]]
    capabilities: Capabilities
    hh_pedal_cc: ControllerSpec | None = None


def parse_profile(data: dict) -> KitProfile:
    """Pure: profile dict → KitProfile. Validates against the frozen vocabulary."""
    if not isinstance(data, dict):
        raise ProfileError("profile must be a JSON object")
    profile_id = data.get("profile_id")
    if not isinstance(profile_id, str) or not profile_id:
        raise ProfileError("profile_id must be a non-empty string")
    name = data.get("name", profile_id)
    version = data.get("profile_version", 1)
    if not isinstance(version, int) or isinstance(version, bool) or version < 1:
        raise ProfileError(f"profile_version must be a positive integer, got {version!r}")

    raw_map = data.get("note_map")
    if not isinstance(raw_map, dict) or not raw_map:
        raise ProfileError("note_map must be a non-empty object")
    note_map: dict[int, tuple[Instrument, str | None]] = {}
    for key, entry in raw_map.items():
        try:
            note = int(key)
        except (TypeError, ValueError):
            raise ProfileError(f"note_map key {key!r} is not an integer") from None
        if not (0 <= note <= 127):
            raise ProfileError(f"note {note} out of MIDI range 0-127")
        if not isinstance(entry, dict):
            raise ProfileError(f"note_map[{key}] must be an object")
        try:
            instrument = Instrument(entry.get("instrument"))
        except ValueError:
            raise ProfileError(
                f"note_map[{key}]: {entry.get('instrument')!r} is not in the frozen "
                f"instrument set (spec 4a)"
            ) from None
        articulation = entry.get("articulation")
        if articulation is not None:
            if articulation not in ARTICULATIONS[instrument]:
                raise ProfileError(
                    f"note_map[{key}]: articulation {articulation!r} invalid for "
                    f"{instrument.value} (allowed: {sorted(ARTICULATIONS[instrument])})"
                )
        note_map[note] = (instrument, articulation)

    caps_raw = data.get("capabilities", {})
    if not isinstance(caps_raw, dict):
        raise ProfileError("capabilities must be an object")
    capabilities = Capabilities(
        has_hh_pedal=bool(caps_raw.get("has_hh_pedal", False)),
        has_positional_sensing=bool(caps_raw.get("has_positional_sensing", False)),
        velocity_curve_id=caps_raw.get("velocity_curve_id"),
    )

    hh_pedal_cc = None
    controllers = data.get("controllers", {})
    if isinstance(controllers, dict) and "hh_pedal" in controllers:
        spec = controllers["hh_pedal"]
        if not isinstance(spec, dict):
            raise ProfileError("controllers.hh_pedal must be an object")
        try:
            hh_pedal_cc = ControllerSpec(
                controller=int(spec["controller"]),
                min=int(spec.get("min", 0)),
                max=int(spec.get("max", 127)),
            )
        except (KeyError, TypeError, ValueError) as exc:
            raise ProfileError(f"controllers.hh_pedal malformed: {exc}") from exc

    return KitProfile(
        profile_id=profile_id,
        name=name,
        profile_version=version,
        note_map=MappingProxyType(note_map),
        capabilities=capabilities,
        hh_pedal_cc=hh_pedal_cc,
    )


def load_profile(path: str | Path) -> KitProfile:
    """Read a profile JSON file from an explicit path."""
    raw = Path(path).read_text(encoding="utf-8")
    try:
        data = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise ProfileError(f"{path}: invalid JSON: {exc}") from exc
    return parse_profile(data)
