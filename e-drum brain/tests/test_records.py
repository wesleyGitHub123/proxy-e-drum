"""S1 tests: canonical schema, serde byte-stability, reader policy."""

from __future__ import annotations

import json

import pytest

from edrum.engine.records import (
    SCHEMA_VERSION,
    BookmarkRecord,
    CtrlRecord,
    EnrollEndRecord,
    EnrollStartRecord,
    EventRecord,
    GridEndRecord,
    GridStartRecord,
    MetaRecord,
    RecordError,
    SchemaVersionError,
    SessionEndRecord,
    UnknownRecord,
    parse_line,
    to_line,
)
from tests.conftest import make_meta

ALL_SAMPLE_RECORDS = [
    make_meta(),
    EventRecord(t=1000, note=38, velocity=100, channel=9),
    EventRecord(t=1000, note=38, velocity=100),  # channel omitted
    CtrlRecord(t=1200, msg={"type": "control_change", "channel": 9, "control": 4, "value": 90}),
    CtrlRecord(t=1300, msg={"type": "note_off", "channel": 9, "note": 42, "velocity": 64}),
    CtrlRecord(t=1400, msg={"type": "sysex", "data": [65, 16, 0]}),
    GridStartRecord(t=2000, bpm=120, subdiv=4, downbeat_t=2000),
    GridEndRecord(t=4000),
    BookmarkRecord(t=4500),
    EnrollStartRecord(t=5000, profile_ref="basic-rock", bpm=120, subdiv=4, downbeat_t=5000),
    EnrollStartRecord(t=5100, profile_ref=None, bpm=120, subdiv=4, downbeat_t=5100),
    EnrollEndRecord(t=7000),
    SessionEndRecord(t=8000),
]


# ---------------------------------------------------------------------------
# round-trip & canonical form
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("record", ALL_SAMPLE_RECORDS, ids=lambda r: type(r).__name__)
def test_roundtrip_byte_identity(record):
    line = to_line(record)
    assert line.endswith(b"\n")
    parsed = parse_line(line)
    assert parsed == record
    assert to_line(parsed) == line  # byte-identical by construction


def test_canonical_form_is_compact_lf_utf8():
    line = to_line(EventRecord(t=1, note=36, velocity=127, channel=9))
    assert line == b'{"type":"event","t":1,"note":36,"velocity":127,"channel":9}\n'


def test_canonical_meta_key_order():
    line = to_line(make_meta())
    keys = list(json.loads(line).keys())
    assert keys == [
        "type",
        "schema_version",
        "session_id",
        "start_iso",
        "kit_profile_id",
        "user_id",
        "calibration_offset_ms",
    ]


def test_canonical_ctrl_msg_order_type_first_then_sorted():
    # construct with deliberately scrambled key order
    rec = CtrlRecord(t=5, msg={"value": 90, "control": 4, "type": "control_change", "channel": 9})
    line = to_line(rec)
    assert (
        line
        == b'{"type":"ctrl","t":5,"msg":{"type":"control_change","channel":9,"control":4,"value":90}}\n'
    )


def test_serialization_is_deterministic():
    rec = GridStartRecord(t=100, bpm=120, subdiv=4, downbeat_t=100)
    assert to_line(rec) == to_line(GridStartRecord(t=100, bpm=120, subdiv=4, downbeat_t=100))


# ---------------------------------------------------------------------------
# validation (construction-time)
# ---------------------------------------------------------------------------

@pytest.mark.parametrize(
    "kwargs",
    [
        dict(t=-1, note=38, velocity=100),
        dict(t=1.5, note=38, velocity=100),  # float t forbidden (integer ms, 3a)
        dict(t=True, note=38, velocity=100),  # bool is not an int
        dict(t=1, note=128, velocity=100),
        dict(t=1, note=-1, velocity=100),
        dict(t=1, note=38, velocity=128),
        dict(t=1, note=38, velocity=100, channel=16),
        dict(t=1, note=None, velocity=100),
    ],
)
def test_event_validation_rejects(kwargs):
    with pytest.raises(RecordError):
        EventRecord(**kwargs)


def test_ctrl_validation():
    with pytest.raises(RecordError):
        CtrlRecord(t=1, msg={"no_type": 1})
    with pytest.raises(RecordError):
        CtrlRecord(t=1, msg={"type": "note_off", "time": 0})  # mido time forbidden
    with pytest.raises(RecordError):
        CtrlRecord(t=1, msg={"type": "x", "bad": {"nested": 1}})
    # negative ints are fine (pitchwheel), lists of ints are fine (sysex)
    CtrlRecord(t=1, msg={"type": "pitchwheel", "channel": 0, "pitch": -8192})
    CtrlRecord(t=1, msg={"type": "sysex", "data": [1, 2, 3]})


def test_grid_start_validation():
    with pytest.raises(RecordError):
        GridStartRecord(t=1, bpm=0, subdiv=4, downbeat_t=1)
    with pytest.raises(RecordError):
        GridStartRecord(t=1, bpm=120, subdiv=0, downbeat_t=1)
    with pytest.raises(RecordError):
        GridStartRecord(t=1, bpm=120, subdiv=4, downbeat_t=-1)


def test_enroll_start_requires_profile_ref():
    with pytest.raises(RecordError):
        EnrollStartRecord(t=1, profile_ref="", bpm=120, subdiv=4, downbeat_t=1)


def test_enroll_start_profile_ref_none_is_anonymous_not_invalid():
    # No label supplied at capture time (gesture/hardware-button/quick-app
    # path) is a legal, honest state — distinct from the empty string, which
    # stays rejected above. The capture log must never invent a name.
    rec = EnrollStartRecord(t=1, profile_ref=None, bpm=120, subdiv=4, downbeat_t=1)
    assert rec.profile_ref is None


def test_enroll_start_anonymous_serializes_to_null():
    line = to_line(EnrollStartRecord(t=4000, profile_ref=None, bpm=120, subdiv=4, downbeat_t=4000))
    assert line == b'{"type":"enroll_start","t":4000,"profile_ref":null,"bpm":120,"subdiv":4,"downbeat_t":4000}\n'


def test_meta_validation():
    with pytest.raises(RecordError):
        make_meta(session_id="")
    with pytest.raises(RecordError):
        make_meta(schema_version="1")
    # kit_profile_id is a reassignable pointer and may be null
    assert make_meta(kit_profile_id=None).kit_profile_id is None
    # calibration may be null (uncalibrated) or a number
    assert make_meta(calibration_offset_ms=12).calibration_offset_ms == 12


# ---------------------------------------------------------------------------
# reader policy
# ---------------------------------------------------------------------------

def test_unknown_fields_ignored_on_known_types():
    line = b'{"type":"event","t":1,"note":36,"velocity":80,"mystery_field":42}\n'
    rec = parse_line(line)
    assert rec == EventRecord(t=1, note=36, velocity=80)


def test_unknown_type_preserved_byte_faithfully():
    line = b'{"type":"pause","t":500,"reason":"idle"}\n'
    rec = parse_line(line)
    assert isinstance(rec, UnknownRecord)
    assert rec.type == "pause"
    assert rec.t == 500
    assert to_line(rec) == line  # lossless re-emission


def test_newer_major_refused():
    obj = json.loads(to_line(make_meta()))
    obj["schema_version"] = SCHEMA_VERSION + 1
    with pytest.raises(SchemaVersionError):
        parse_line(json.dumps(obj).encode())


def test_older_major_accepted_if_valid():
    # an old file stays readable under its own version (spec §3)
    # (version 1 is the oldest that exists; equality path is the guarantee here)
    rec = parse_line(to_line(make_meta(schema_version=SCHEMA_VERSION)))
    assert isinstance(rec, MetaRecord)


@pytest.mark.parametrize(
    "line",
    [b"", b"   \n", b"not json\n", b"[1,2,3]\n", b'{"t":1}\n', b'{"type":5,"t":1}\n'],
)
def test_malformed_lines_rejected(line):
    with pytest.raises(RecordError):
        parse_line(line)


def test_parse_accepts_str_and_crlf():
    rec = parse_line('{"type":"bookmark","t":9}\r\n')
    assert rec == BookmarkRecord(t=9)
