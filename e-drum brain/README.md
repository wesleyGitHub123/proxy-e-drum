# edrum — the brain half (Phase 0)

Python analysis engine + laptop-phase recorder for the e-drum practice tool.
The product contract is `edrum-brain-spec.md`; the Phase 0 implementation
contract and rationale is `phase0-plan.md`. Read those before changing
anything load-bearing.

**Phase 0 ships:** the Jamcorder-equivalent recorder on the laptop — canonical
session schema, append-only crash-survivable log, live MIDI capture with
arrival-edge timestamps, file replay, TD-02K kit profile, byte-identical
round-trip validation.

## Setup

```powershell
cd "e-drum brain"
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -e ".[dev,live]"
```

`[live]` (python-rtmidi) is only needed for real MIDI ports (`record`/`ports`);
everything else — engine, file tools, tests — works without it.

## Commands

```powershell
.\.venv\Scripts\edrum.exe ports                      # list MIDI inputs
.\.venv\Scripts\edrum.exe record --port "TD-02 0"    # record until Ctrl+C -> sessions/*.jsonl
.\.venv\Scripts\edrum.exe replay sessions\FILE.jsonl # Phase 0 gate: byte-identical round-trip
.\.venv\Scripts\edrum.exe dump sessions\FILE.jsonl --normalized   # lanes via kit profile
```

## Tests

```powershell
.\.venv\Scripts\python.exe -m pytest            # full suite
.\.venv\Scripts\python.exe -m pytest tests\test_records.py -k byte   # one file / pattern
```

`tests/fixtures/*.jsonl` are **byte-frozen golden files** — the executable
specification of the session-file format and the conformance suite any future
producer (the ESP32 capture box included) must pass. Do not regenerate them
casually; a serde change that breaks them is a corpus-format break.

## Layout

```
edrum/engine/   pure: records (schema+serde), session (fold), vocab, profiles, normalize
edrum/io/       clock (the one clock), logfile (append-only), sources (Source seam), capture
edrum/cli/      thin caller: record / replay / dump / ports
profiles/       kit profiles (data, not code) — td02k.json is profile #1
sessions/       recorded session logs (created on first record)
```

Import rule: `io/` may import `engine/`; `engine/` never imports `io/` or `cli/`.
