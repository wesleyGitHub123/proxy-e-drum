"""S8 tests: CLI smoke via main() — replay/dump paths (record needs hardware)."""

from __future__ import annotations

import pytest

from edrum.cli.main import default_profiles_dir, main
from edrum.engine.records import (
    CtrlRecord,
    EventRecord,
    GridEndRecord,
    GridStartRecord,
)
from edrum.io.logfile import SessionWriter
from tests.conftest import make_meta


@pytest.fixture
def session_file(tmp_path):
    path = tmp_path / "s.jsonl"
    w = SessionWriter(path, make_meta())
    w.append(EventRecord(t=10, note=36, velocity=90, channel=9))
    w.append(
        CtrlRecord(t=15, msg={"type": "control_change", "channel": 9, "control": 4, "value": 30})
    )
    w.append(GridStartRecord(t=1000, bpm=120, subdiv=4, downbeat_t=1000))
    w.append(EventRecord(t=1500, note=38, velocity=100, channel=9))
    w.append(GridEndRecord(t=2000))
    w.append(EventRecord(t=2500, note=99, velocity=50, channel=9))  # unmapped pad
    w.close(end_t=3000)
    return path


def test_replay_passes_on_clean_file(session_file, capsys):
    assert main(["replay", str(session_file)]) == 0
    out = capsys.readouterr().out
    assert "PASS byte-identical round-trip" in out
    assert "1 grid segment" in out


def test_replay_reports_recovery_on_torn_file(session_file, tmp_path, capsys):
    data = session_file.read_bytes()
    torn = tmp_path / "torn.jsonl"
    torn.write_bytes(data[:-10])
    assert main(["replay", str(torn)]) == 0  # recovered prefix still round-trips
    out = capsys.readouterr().out
    assert "crash-recovered" in out
    assert "PASS" in out


def test_dump_plain(session_file, capsys):
    assert main(["dump", str(session_file)]) == 0
    out = capsys.readouterr().out
    assert "kit       td02k" in out
    assert "grid      [1000..2000] bpm=120" in out
    assert "t=      10 note= 36" in out


def test_dump_normalized_uses_td02k_profile(session_file, capsys):
    assert main(["dump", str(session_file), "--normalized"]) == 0
    out = capsys.readouterr().out
    assert "KICK [foot]" in out
    assert "SNARE.head [hands_unresolved]" in out
    assert "UNMAPPED" in out  # note 99: visible lane, never a crash


def test_dump_limit(session_file, capsys):
    assert main(["dump", str(session_file), "--limit", "1"]) == 0
    out = capsys.readouterr().out
    assert "more event(s)" in out


def test_default_profiles_dir_contains_td02k():
    assert (default_profiles_dir() / "td02k.json").is_file()


# ---------------------------------------------------------------------------
# analyze (S13′)
# ---------------------------------------------------------------------------

def test_analyze_text_output(session_file, capsys):
    assert main(["analyze", str(session_file)]) == 0
    out = capsys.readouterr().out
    assert "timing.rush_drag" in out
    assert "span[1000..2000] bpm=120 subdiv=4" in out
    assert "(uncalibrated)" in out
    assert "lane=SNARE.head" in out  # profile resolved from meta pointer


def test_analyze_grid_empty_refuses_with_exit_0(tmp_path, capsys):
    path = tmp_path / "free.jsonl"
    w = SessionWriter(path, make_meta())
    w.append(EventRecord(t=10, note=36, velocity=90, channel=9))
    w.close(end_t=100)
    assert main(["analyze", str(path)]) == 0  # absence of grid is truth, not error
    out = capsys.readouterr().out
    assert "refused   timing.deviations: no_grid_segments" in out
    assert "velocity.per_note" in out  # grid-free tier still present


def test_analyze_json_round_trips(session_file, capsys):
    import json

    assert main(["analyze", str(session_file), "--json"]) == 0
    d = json.loads(capsys.readouterr().out)
    assert d["analysis_version"] == 1
    assert any(r["analyzer_id"] == "timing.deviations" for r in d["results"])


def test_analyze_metrics_subset_and_unknown(session_file, capsys):
    assert main(["analyze", str(session_file), "--metrics", "timing.rush_drag"]) == 0
    out = capsys.readouterr().out
    assert "timing.rush_drag" in out
    assert "velocity" not in out  # subset honored (upstream dep runs silently)

    assert main(["analyze", str(session_file), "--metrics", "nope.nothing"]) == 1
    out = capsys.readouterr().out
    assert "unknown metric" in out and "timing.rush_drag" in out


def test_analyze_kit_override_repoints_lanes(session_file, capsys):
    """--kit is the re-pointable pointer (§4.2); a bogus explicit id errors."""
    assert main(["analyze", str(session_file), "--kit", "no-such-kit"]) == 1
    out = capsys.readouterr().out
    assert "error" in out
