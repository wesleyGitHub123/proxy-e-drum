# Phase 0 Implementation Plan — APPROVED (implementation contract)

*Approved 2026-07-06. This document is the implementation contract for Phase 0 and the
rationale record behind its architecture. It must stay synchronized with the code. The
product contract remains `edrum-brain-spec.md` (+ `edrum-capture-spec.md`); this plan
executes brain-spec §9 Phase 0 and changes nothing architectural.*

---

## Phase 0 scope (brain spec §9)

Lock the §3 schema (versioning, append-only typed-line log, `ctrl` atom, per-session
`calibration_offset_ms`, integer-ms representation), TD-02K kit profile, engine/io/cli
scaffold, `LiveMidiSource` + `FileSource`, laptop-phase capture half owning one clock
(capture spec §4A). Validated by byte-identical round-trip + truncate-to-last-complete-line
recovery. Ships the Jamcorder-equivalent recorder, on the laptop.

## Dependencies

| Dependency | Why | Phase 0? |
|---|---|---|
| Python ≥ 3.11 | dataclasses/typing | Yes |
| `mido` + `python-rtmidi` | `LiveMidiSource` (spec §2); rtmidi is the `[live]` extra | Yes |
| `pytest` | test runner (`[dev]` extra) | Yes |
| TD-02 MIDI implementation chart | profile #1 note map — "read, not guessed" | **Provided — see below** |
| `sounddevice` (or similar) | click audio callback | No — Phase 1. Clock design anticipates it |
| numpy/scipy | Layer A math | No — Phase 1 |

## Layout (spec §2, unchanged)

```
e-drum brain/
  pyproject.toml
  phase0-plan.md          (this file)
  edrum/
    engine/   records.py  session.py  vocab.py  profiles.py  normalize.py
    io/       clock.py  logfile.py  sources.py  capture.py
    cli/      main.py
  profiles/   td02k.json
  tests/      test_*.py  fixtures/*.jsonl
```

Rule enforced structurally: `io/` may import `engine/`; `engine/` never imports `io/` or
`cli/` (engine purity, brain spec §2).

---

## Approved micro-decisions (FINAL — these freeze bytes)

1. **`ctrl` payload encoding** — structured mido-style dict, e.g.
   `{"type":"control_change","channel":9,"control":4,"value":90}`. 1:1 with wire bytes for
   channel messages; the mido `time` field is stripped (our `t` is authoritative). Within
   `msg`, canonical key order is `type` first, then remaining keys sorted alphabetically.
2. **System-realtime filter** — `active_sensing`, `clock`, `start`, `stop`, `continue`,
   `reset` are dropped at capture. Documented, principled exception to "preserve
   losslessly": these carry transport keepalive, not performance information (Roland
   modules emit active-sense ~3×/sec — thousands of zero-information lines per session).
   All channel-scoped messages + sysex are kept.
3. **Note-off / note-on-velocity-0** — recorded as `ctrl` records (preserved-but-invisible,
   exactly that atom's role; brain spec §3). Never `event`s (would pollute onset analysis),
   never dropped (would violate lossless).
4. **`schema_version`** — single integer = *major* compatibility only. Minor evolution needs
   no number: additive changes are self-describing under the ignore-unknown-fields reader
   policy. Reader refuses `schema_version` greater than it supports. Current: `1`.
5. **Session file naming** — `sessions/YYYYMMDDTHHMMSS_<id8>.jsonl` (timestamp from
   `start_iso` local time; `id8` = first 8 chars of the session UUID hex).
6. **Canonical serialization (frozen)** — UTF-8, no BOM, LF line endings, compact
   separators (`,` `:`), deterministic per-type key order (defined in
   `engine/records.py`), integers where the schema says integer. Guarded by
   golden-byte tests.

## TD-02K verified MIDI implementation (approved input)

| Pad / zone | Note | → (instrument, articulation) |
|---|---|---|
| Kick | 36 | KICK |
| Snare Head | 38 | SNARE, head |
| Snare Rim | 40 | SNARE, rim |
| Snare X-Stick | 37 | SNARE, crossstick |
| Tom 1 | 48 | TOM_HIGH, head |
| Tom 2 | 45 | TOM_MID, head |
| Tom 3 | 43 | TOM_LOW, head |
| Hi-Hat Open Bow | 46 | HIHAT, open |
| Hi-Hat Open Edge | 26 | HIHAT, open |
| Hi-Hat Closed Bow | 42 | HIHAT, closed |
| Hi-Hat Closed Edge | 22 | HIHAT, closed |
| Hi-Hat Pedal | 44 | HH_PEDAL |
| Crash 1 Bow | 49 | CRASH, bow |
| Crash 1 Edge | 55 | CRASH, edge |
| Ride Bow | 51 | RIDE, bow |
| Ride Edge | 59 | RIDE, edge |
| Ride Bell | 53 | RIDE, bell |

Hi-hat continuous controller: **CC 4, value range 0–90** — stored in the profile
(`controllers.hh_pedal`) for the future openness derivation (brain spec §4.1); Phase 0
preserves the CC stream as `ctrl` records and derives nothing from it yet.

*Note on HH bow/edge:* the frozen HIHAT articulation set is `{open, half, closed}`
(spec §4.1 decision 4a), so bow/edge pairs map to the same articulation. No information is
lost — the raw note is preserved, and a future *additive* articulation extension can
re-derive the distinction from history for free (spec §4.2). `half` arrives later, derived
from CC4 via `ctrl`.

---

## Subsystems

### S1 — Canonical records & serialization (`engine/records.py`)

- **Purpose:** the typed record vocabulary and its one canonical byte form. This *is* the firmware↔brain contract.
- **Responsibilities:** line types `meta`, `event`, `ctrl`, `grid_start`, `grid_end`, `bookmark`, `enroll_start`, `enroll_end`, `session_end`; construction-time validation (integer `t ≥ 0`, `note`/`velocity` 0–127); canonical serde per micro-decision 6; reader policy (ignore unknown fields; refuse newer major; unknown line *types* → `UnknownRecord`, preserved byte-faithfully and ignored semantically).
- **Non-responsibilities:** no file I/O; no folding; no normalization; no MIDI parsing.
- **Interface:** `parse_line(line) -> Record`, `to_line(Record) -> bytes` (incl. `\n`), record dataclasses, `SCHEMA_VERSION`.
- **Data contract:** meta = `{schema_version, session_id, start_iso, kit_profile_id, user_id, calibration_offset_ms|null}`; event = `{type,t,note,velocity[,channel]}`; ctrl = `{type,t,msg}`; grid_start = `{type,t,bpm,subdiv,downbeat_t}`; enroll_start adds `profile_ref`. Declarations fully schematized now (spec §3: "the schema decision is the irreversible part").
  - **Amendment (2026-07-11, schema v2):** `profile_ref` is nullable — `null` = no label supplied at capture time (anonymous enrollment; brain spec §3/§13). This is the one field-meaning change against the S1 contract as originally frozen here; `SCHEMA_VERSION` moved 1 → 2 accordingly (brain spec §3 schema-evolution rule). The rest of S1's frozen contract (key order, byte form, reader policy) is unchanged.
- **Extension points:** new line types / fields are additive; `pause`/`resume` reserved via unknown-type tolerance.
- **Migration risk:** highest in project (permanent corpus). Avoidance: integer major version + ignore-unknown; golden fixtures freeze bytes; files never rewritten.

### S2 — Log writer & recovering reader (`io/logfile.py`)

- **Purpose:** the append-only on-disk log (§3; capture spec §6).
- **Responsibilities:** writer appends canonical lines only — no seek, never rewrites line 1, flush per line, asserts non-decreasing `t`. Reader streams lines, truncates trailing incomplete/invalid line (crash recovery), delegates parsing to S1. Filename convention (micro-decision 5).
- **Non-responsibilities:** no semantics (S3's job); reader never repairs files on disk; writer never reads.
- **Interface:** `SessionWriter(path, meta)` / `.append(record)` / `.close(end_t)` / `.abort()`; `read_log(path) -> LogRead(meta, records, recovery)`; `load_session(path)` convenience (io may import engine).
- **Acceptance:** kill-mid-write simulation → reader returns valid session up to last complete line.

### S3 — Session reduction (`engine/session.py`)

- **Purpose:** fold the record stream into the §3 Session object (the "parsed reduction").
- **Responsibilities:** pair `grid_start`/`grid_end` into `GridSegment`s (a `grid_start` while a span is open closes it at the new start — the §5 param-change rule); same pairing for enrollment; auto-close dangling spans at the last event within them (§5), degenerate to `start_t` if the span holds no events; collect bookmarks; warn (never fail) on unmatched ends / non-monotonic `t` from foreign producers; ignore unknown records (tallied in warnings).
- **Non-responsibilities:** no grading, no normalization, no I/O; never writes derived state (invariant: grading is derived, never stored).
- **Interface:** `reduce_session(meta, records) -> Session`.
- **Acceptance:** declaration fixtures fold correctly; a warmup-with-click log with no declarations yields an empty grid track.

### S4 — Session clock (`io/clock.py`)

- **Purpose:** the *one clock* (capture spec §3 Rule 1, §4A) — single time authority for stamps now and click scheduling in Phase 1.
- **Interface:** `Clock` protocol: `now_ms() -> int` (session-relative, monotonic, never rebased), `start_iso`. `SessionClock` (perf_counter_ns anchor) + `FakeClock` (injectable; part of the testability contract, not test-only).
- **Extension point:** the Phase 1 click scheduler consumes the same instance.

### S5 — Source abstraction (`io/sources.py`)

- **Purpose:** spec §2's `Source` seam — brain consumes live and file identically.
- **Responsibilities:** `MessageStamper` — stamps `t` FIRST (arrival edge), then classifies: realtime types dropped (micro-decision 2); `note_on` v>0 → `event`; everything else → `ctrl` with mido-style `msg` minus `time` (micro-decisions 1, 3). `LiveMidiSource` stamps in the mido callback thread and enqueues (`SimpleQueue` = the laptop ring buffer); `FileSource` replays via S2 reader; `FakeSource` is scripted (the test workhorse). `new_session_meta()` builds meta at session start.
- **Non-responsibilities:** no writing, no analysis, no normalization, no gesture detection (Phase 3).
- **Interface:** `Source` protocol: `open() -> MetaRecord`, `events() -> Iterator[Record]`, `close()`.
- **Testing note:** python-rtmidi virtual ports are unavailable on Windows, so `LiveMidiSource` is unit-tested by driving `_on_message` / `MessageStamper` directly with constructed `mido.Message` objects and a `FakeClock` — no ports needed.
- **Extension point:** the box's files arrive through `FileSource` unchanged — that is the entire hardware migration story.

### S6 — Vocabulary, kit profile, normalization (`engine/vocab.py`, `profiles.py`, `normalize.py`, `profiles/td02k.json`)

- **Purpose:** §4.1 frozen lanes + additive articulation; §4.2 brain-side re-runnable normalization.
- **Responsibilities:** frozen `Instrument` enum (exactly the ten of decision 4a — guard-tested); per-instrument articulation sets; `KitProfile` = note map + capability flags + `hh_pedal_cc`; total `normalize_event` (unknown note → `UNMAPPED`, never raises); limb class (`KICK`/`HH_PEDAL` → foot, all else → hands-unresolved — the §7 hand-identity wall is enforced at the type level: no LEFT/RIGHT anywhere).
- **Non-responsibilities:** no velocity-curve correction (Phase 6); no `ctrl`-derived articulation yet (additive later); derived views never persisted.
- **Interface:** `parse_profile(dict) -> KitProfile`, `load_profile(path)`, `normalize_event(event, profile) -> NormalizedView`.

### S7 — Capture host pipeline (`io/capture.py`)

- **Purpose:** capture spec §4A — the host process wearing its capture hat; producer #1 of session files.
- **Responsibilities:** mirrors firmware §2 — callback thread stamps + enqueues (in S5); this loop drains the queue into the writer. Stamping can never be smeared by a slow write. Clean stop writes `session_end`; unclean stop survivable by construction (S2).
- **Non-responsibilities:** no click yet (Phase 1, same clock instance); no gestures (Phase 3); no analysis, ever.
- **Interface:** `run_capture(source, writer, on_record=None) -> count`.

### S8 — CLI (`cli/main.py`)

- **Purpose:** the thin caller (§2). `record`, `replay` (round-trip byte-identity check), `dump [--normalized]`, `ports`.
- **Non-responsibilities:** zero logic — any conditional more interesting than arg handling belongs in engine/io.

### Golden fixtures (`tests/fixtures/`)

Checked-in canonical logs: minimal session; crash-truncated variant; declarations-bearing
variant (grid + enrollment + bookmark); warmup-with-click-no-grid variant; forward-compat
variant (unknown fields + unknown line type). The validation gate a phase passes becomes
its permanent regression test (spec §10) — and this corpus **is the conformance suite the
ESP32 firmware must satisfy byte-for-byte later**. The interface between firmware and
brain is the file; the fixtures are its executable specification.

---

## Dependency graph

```
S1 records/serde ──┬─→ S2 logfile ──┬─→ S3 session fold ─→ S8 CLI(dump/replay)
                   │                ├─→ S5 FileSource
                   │                └─→ S7 capture pipeline ─→ S8 CLI(record)
                   ├─→ S5 sources ──────→ S7
                   └─→ S6 vocab/profile ─→ S8 CLI(--normalized)
S4 clock ──────────────→ S5 LiveMidiSource, S7
fixtures ← S1+S2+S3
```

**Build order: S1 → S4 → S2 → S3 → S6 → S5 → S7 → S8 → fixtures/e2e.** S1 first because
every subsystem imports it, the byte-identical acceptance gate is undefined until serde is
frozen, and it is the only subsystem where a mistake forces corpus migration.

## Future-phase non-violation audit

| Future phase | Constraint on Phase 0 | How honored |
|---|---|---|
| P1 Layer A | click shares stamp clock; per-session calibration | S4 single time authority; meta field exists (null = uncalibrated) |
| P2 Layer D | corpus = the logs, dated | `start_iso` in meta; one file per session; no second format |
| P3 declarations | line types + fold semantics exist | fully schematized in S1/S3; auto-close + click-snapshot fields in place |
| P4 Layer B | pure engine over `Session` | S3 output is the stable input |
| P5 Layer C | `enroll_start` carries grid snapshot | in S1 schema now (spec 3f) |
| P6 multi-drummer | profiles are data; `kit_profile_id` re-pointable | profile = JSON file; id in meta, read at view time, never baked into events |
| The box | must emit identical files | fixtures + round-trip suite = its acceptance test |

## Vertical slices

- **Slice 1 (no hardware):** S1+S2+S3 + `FakeSource` + fake clock through the real capture
  pipeline: scripted hits → pipeline → `session.jsonl` → recovering reader → fold →
  re-serialize → **byte-compare**; plus the truncation crash sweep. Its output is golden
  fixture #1 and the eventual firmware conformance test.
- **Slice 2 (hardware smoke):** swap in `LiveMidiSource` + real TD-02K — hit pads, verify
  sane monotonic stamps, record and replay a real session. *(Manual step — requires the
  kit connected; `edrum record` is the entry point.)*

## Status

- [x] Plan approved, micro-decisions 1–6 finalized, TD-02K chart data provided
- [x] S1–S8 implemented with unit tests (2026-07-06)
- [x] Golden fixtures + Slice-1 e2e green (incl. crash-cut sweep over every byte offset)
- [ ] Slice 2: live smoke test against the physical TD-02K (manual; needs the kit connected —
      `edrum ports`, then `edrum record --port <TD-02 port>`, hit pads, Ctrl+C,
      `edrum replay sessions\<file>.jsonl` must PASS)
