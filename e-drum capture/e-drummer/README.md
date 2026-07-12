# e-drummer capture firmware

ESP32-S3 firmware for the capture half of the e-drum practice tool: host the
TD-02 over USB, timestamp every MIDI arrival on one clock, generate the
click, and persist the append-only session log the brain consumes
([edrum-capture-spec.md](edrum-capture-spec.md); brain contract:
`e-drum brain/edrum-brain-spec.md` §3).

The architecture mirrors the brain's Phase 0 discipline: a pure core, ports
for everything hardware-shaped, adapters at the edge, and byte-frozen golden
fixtures as the executable contract.

```
lib/edrum_core/        PURE C++14 — no Arduino/ESP-IDF anywhere (the "engine")
  edrum/records.h        in-RAM record model (the type every subsystem meets at)
  edrum/serialize.*      canonical JSONL bytes  ← byte-identical to the brain
  edrum/ring.h           lock-free SPSC ring (capture ↔ storage seam)
  edrum/usbmidi.*        USB-MIDI event-packet parser (+ sysex assembly)
  edrum/classify.h       realtime filter / event-vs-ctrl split (mido mirror)
  edrum/descwalk.*       config-descriptor walk → MIDI Streaming iface + EPs
  edrum/session_fsm.*    session lifecycle: auto-start, pause cache, idle end
  edrum/logwriter.*      storage policy: open/meta/append/sync/close
  edrum/click_sched.h    beat-edge arithmetic (integer, drift-free)
  edrum/click_render.*   click → 1 ms PCM blocks
  edrum/gesture.*        kick+crash chord grammar (bookmark / grid / enroll toggle)
  edrum/control_msg.h    ControlMsg/ControlResult — the source-agnostic control vocabulary
  edrum/control_dispatch.* ControlDispatcher: ControlMsg → SessionController (the one caller)
  edrum/frame.*          COBS + CRC-16 wire framing (brain mirror: io/framing.py)
  edrum/sync_service.*   session-archive sync protocol, device side (spec §13)
  edrum/console_cmd.*    console grammar (parsing only)
  edrum/config.*         key=value /config.txt parser
  edrum/timefmt.*        ISO-8601, epoch math, session filenames, uuid4
  edrum/hal/ports.h      IClock IWallClock IRandom IStorage IAudioOut
                         IByteLink ISessionArchive IClickSnapshot

lib/edrum_platform/    ESP32-only adapters (the ONLY hardware code)
  esp32_clock.h          esp_timer → IClock          (the one clock, spec §3)
  usb_host_midi.*        native USB host → stamped MidiMsg callbacks
  sd_storage.h           SdFat SPI microSD → IStorage + ISessionArchive
  esp32_wallclock.h      system time + NVS ordering checkpoint → IWallClock
  i2s_click_out.h        legacy I2S driver → IAudioOut (DAC → TD-02 MIX IN)
  serial_link.h          console line → IByteLink (sync transport #1, spec §13)
  esp32_random.h         esp_random → IRandom

src/                   composition root + tasks (thin; zero domain logic)
  main.cpp               construct once, wire ports, start tasks
  capture_task.cpp       core 0: USB poll → classify → FSM → ring  [producer]
  storage_task.cpp       core 1: ring → LogWriter → SD; sync pump  [consumer]
  click_task.cpp         core 0: schedule → I2S (tempo authority)
  console.cpp            core 1 (loop): serial console (modal ↔ sync)

test/native/           runs on the laptop, no hardware: pio test -e native
test/embedded/         on-target smoke: pio test -e esp32-s3-devkitc-1
tools/probe/           the validated enumeration probe (Experiment 2 artifact)
tools/gen_fixture_header.py   embeds the brain's golden fixtures into the
                              conformance test (single source of truth)
```

## Data flow (the one picture)

```
TD-02 ──USB──► usb_host_midi ──stamp t FIRST──► parser ► classify ─┬─► session FSM ─► ring
                                                                   │      ▲    ▲
console (loop, core 1) ──ControlMsg queue──► ControlDispatcher ────┘      │    │
gestures (kick+crash chords) ──ControlMsg (in-process)──► ControlDispatcher ───┘
click task ◄─ click_sched (same IClock) — declarations snapshot it ────────────┘

Any future controller (desktop/mobile app, hardware button, BLE, network
API) is just another ControlMsg producer feeding the same ControlDispatcher
— the session FSM never sees a controller, only FSM method calls.

ring ──► storage task ──► LogWriter ──► SdFat ──► /sessions/YYYYMMDDTHHMMSS_<id8>.jsonl
                                                   └─ readable by `edrum replay` / `dump`
```

Every producer funnels through the capture task (single ring producer =
lock-free SPSC + total timeline order). Storage latency can never smear a
timestamp: stamps happen on the other side of the ring.

## Build / test / flash

```bash
pio test -e native            # every core subsystem, incl. golden-fixture
                              # byte conformance — no hardware needed
pio run                       # build firmware
pio run --target upload       # flash over the UART/COM port (kit stays hosted)
pio device monitor            # 115200 baud console
pio test -e esp32-s3-devkitc-1   # on-target smoke test
```

## Console

`help` prints the grammar. Highlights:

| command | effect |
|---|---|
| `stats` | health + experiment counters (ring high-water, drops, stalls, USB gaps) |
| `settime 2026-07-10T21:30:00+08:00` | set wall clock (session dating, ADR-5) |
| `click 96` / `click off` | start/stop the click (device = tempo authority) |
| `grid start` / `grid end` | declare a graded span (snapshots the running click) |
| `bookmark`, `enroll <name>`, `enroll end`, `end` | declarations / session end |
| `burst 2000 500` | Experiment 3: synthetic events through the real path |
| `sync` | hand the line to the host sync protocol (`edrum sync`); console returns on BYE / 10 s idle |

Gestures (during play, no keyboard): **kick+crash ×2** = bookmark,
**kick+crash ×3** = grid toggle.

## /config.txt (on the SD card, optional)

```ini
# e-drummer config — key = value, '#' comments, unknown keys tolerated
user_id = wesley
kit_profile_id = td02k          # re-pointable hint, never a welded fact
calibration_offset_ms = null    # per-session meta (capture spec §4)
tz_offset_min = 480             # +08:00
click_bpm = 120
click_subdiv = 4
click_gain = 50                 # 0-100 % into TD-02 MIX IN (this chain's knob,
                                # firmware-local — never brain/profile data)
pause_after_ms = 3000           # event silence before ctrl-caching
idle_end_ms = 300000            # idle failsafe: session_end after 5 min
gesture_enable = on
gesture_note_a = 36             # kick
gesture_note_b = 49             # crash 1 bow
```

## Pin map (defaults — ⚠ verify at wiring time, `src/app_config.h`)

| function | GPIO |
|---|---|
| SD CS / MOSI / SCK / MISO | 10 / 11 / 12 / 13 |
| I2S BCLK / LRCK / DOUT | 14 / 15 / 16 |
| kit | native USB-OTG port (D− 19 / D+ 20) + external VBUS (spec §8.2) |
| flash/monitor | the UART USB port (spec §8.3) |

Chosen to avoid USB (19/20), flash (26–32), octal PSRAM (33–37), strapping
(0/3/45/46), and UART0 (43/44) pins.

## Experiment runbook (firmware-architecture-roadmap.md)

* **Experiment 1 (ADR-1, USB timestamp path)** — plug the kit, play a
  sustained fast roll, read `stats`. Success = no missed events (`ring
  DROPS=0`, `realtime_dropped=0`, high-water « capacity) **and** a tight
  lower tail: `cb_gap min` / `burst(<=15ms) max` near the ~1 ms frame floor
  with the `gap hist` showing no pile-up at a host quantum. Ignore `cb_gap
  max` — a big idle gap is just a rest (an idle IN endpoint NAKs), not
  latency.
* **Experiment 3 (ADR-3, storage stall budget)** — `burst 5000 1000`, then
  `stats`: `ring high_water` (buffer depth actually needed), `DROPS` (must
  be 0), `stall max` / `>50ms`. Also yank power mid-burst and confirm
  `edrum replay` recovers the file to the last complete line.
* **Experiment 5 (ADR-5, session dating)** — cold-boot cycles with and
  without `settime`; confirm session files still sort by name/`start_iso`.
* **Experiment 6 (ADR-6, concurrency)** — play + click + burst
  simultaneously; watch `DROPS`, `click late_max`, watchdog silence.

## Syncing sessions to the brain (spec §13)

The box is the writer-of-record; the brain replicates its archive
**byte-exactly** — a synced file is indistinguishable from a card-reader
copy (which remains the zero-code fallback transport). From the brain
directory, with the `[device]` extra installed (`pip install -e ".[device]"`):

```powershell
.\.venv\Scripts\edrum.exe sync --port COM5          # -> sessions\*.jsonl
```

The CLI types `sync` into the console itself, handshakes (injecting host
wall time — every sync heals ADR-5 dating), lists closed sessions, fetches
what's missing (resumable, CRC-verified), and never overwrites an existing
local file. Dead/full/corrupt card: the box keeps capturing and clicking
(fail-soft; `stats` shows `discarded` + `[FAIL-SOFT]`), and sync reports an
explicit storage-failed error instead of hanging. Wire discipline (COBS +
CRC-16) is mirrored in `edrum/io/framing.py`; the shared test vectors
(`test_frame` ↔ `test_devlink.py`) are the cross-language contract.

v1 limits (documented): closed sessions only; `LIST` caps at 64 files —
sync (or archive off) the card before it accumulates more.

## Conformance to the brain (the seam)

`test/native/test_conformance` rebuilds the brain's byte-frozen golden
fixtures (`e-drum brain/tests/fixtures/*.jsonl`) through this firmware's
serializer and compares **whole files byte-for-byte** — the fixtures header
is generated from the brain's copies by `tools/gen_fixture_header.py`, never
hand-copied. The end-to-end gate for the physical box stays what the spec
says it is: record a real session, sync (or card-copy) it over, then

```powershell
.\.venv\Scripts\edrum.exe replay sessions\<file>.jsonl    # byte round-trip PASS
.\.venv\Scripts\edrum.exe dump sessions\<file>.jsonl --normalized
```

## v1 restrictions (deliberate, documented)

* All numerics emitted are integers (bpm, calibration) — sidesteps
  cross-language float formatting; the schema itself allows floats.
* Sysex payloads cap at 48 data bytes (truncated + counted; the TD-02 sends
  no unsolicited bulk sysex during performance).
* Click assumes 4/4 accents; `subdiv` rides the grid records for the brain.
* ADR-5 (dating) ships as ordering-guaranteed + `settime`; a battery RTC
  later replaces one adapter (`esp32_wallclock.h`) and nothing else.
