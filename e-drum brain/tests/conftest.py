"""Shared test helpers."""

from __future__ import annotations

from pathlib import Path

import pytest

from edrum.engine.records import MetaRecord, SCHEMA_VERSION

FIXTURES_DIR = Path(__file__).parent / "fixtures"


def make_meta(**overrides) -> MetaRecord:
    """A valid MetaRecord with deterministic defaults; override any field."""
    fields = dict(
        schema_version=SCHEMA_VERSION,
        session_id="a1b2c3d4e5f60718293a4b5c6d7e8f90",
        start_iso="2026-07-06T12:00:00+08:00",
        kit_profile_id="td02k",
        user_id="local",
        calibration_offset_ms=None,
    )
    fields.update(overrides)
    return MetaRecord(**fields)


@pytest.fixture
def meta() -> MetaRecord:
    return make_meta()
