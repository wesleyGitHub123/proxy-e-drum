# Milestone Plan — First End-to-End Prototype: PCM5102 Audio Bring-Up + the Device↔Brain Link

*APPROVED 2026-07-12 (five decisions resolved below) and IMPLEMENTED the same day —
110 firmware native tests + ESP32 build green, 140 brain tests green. The resolved
decisions have migrated into the living specs (capture spec **§13** + §4, roadmap
**ADR-7** + Experiments 7/8, brain spec §2, firmware README); those are authoritative
now — this file is the milestone's rationale record, the way `phase0-plan.md` is for
Phase 0. Remaining to close the milestone: on-hardware Experiments 7 and 8, then the
Part IV step-5 end-to-end gate.*

**Milestone intent:** the core capture pipeline (USB host, timestamping, ring, SD,
append-only log) is validated. This milestone turns the box into the first true
end-to-end prototype: (I) the click is heard through the PCM5102 → TD-02 MIX IN, and
(II) a session recorded on the box lands in the brain's corpus and passes the Phase 0
byte-identity gate — over a communication layer whose *interfaces* are the deliverable,
so BLE/Wi-Fi/Ethernet later plug into the same seams.

---

## Part I — Audio subsystem (PCM5102 I2S DAC)

### I.1 Review finding: the click path is already integrated, not a bolt-on

The requested review (metronome, click generation, scheduler, configuration, layering)
finds the audio subsystem **already implemented in exactly the layering the spec asks
for**. Nothing about its placement needs to change:

| Piece | Where | Layer discipline |
|---|---|---|
| `ClickScheduler` — integer beat-edge arithmetic, drift-free (each edge computed from the anchor), snapshot source for declarations | `edrum_core/click_sched.h` | pure, natively tested (`test_click`) |
| `synth_click` — decaying sine burst, accent/normal voices, rendered once at init | `edrum_core/click_wave.*` | pure |
| `ClickRenderer` — mixes scheduled edges into 1 ms PCM blocks; every block re-reads `IClock`, so sample-clock vs `esp_timer` drift self-corrects to ±1 block | `edrum_core/click_render.*` | pure |
| `hal::IAudioOut` — mono 16-bit PCM sink; blocking `write()` is the pacing | `edrum_core/hal/ports.h` | port |
| `hal::IClickSnapshot` — declarations snapshot the *running* click without touching platform state | `edrum_core/hal/ports.h` | port |
| `I2sClickOut` — legacy IDF I2S driver, 4×64-frame DMA (~5.3 ms constant → `calibration_offset_ms`) | `edrum_platform/i2s_click_out.h` | the one hardware file |
| `click_task` — core 0, prio 11 (tightest deadline, tiny work) | `src/click_task.cpp` | composition root |
| `click <bpm> [subdiv]` / `click off`; `click_bpm`/`click_subdiv` defaults | console / `/config.txt` | control surface / config |

The one-clock rule is structural: the same `Esp32Clock` instance feeds the capture stamp
path, the scheduler, and the renderer's block decisions. Grid/enroll declarations
snapshot the schedule through `IClickSnapshot`, so "the grid is exact-by-construction"
holds on the box exactly as specced (capture spec §4).

**Consequence:** this half of the milestone is **hardware bring-up plus three small
gaps**, not new architecture. Proposing a new "audio subsystem" would duplicate a
validated design.

### I.2 PCM5102 bring-up items (hardware, ⚠ VERIFY against the actual breakout)

The adapter deliberately emits **no MCLK** (`mck_io_num = I2S_PIN_NO_CHANGE`). The
PCM5102A supports this — but only in PLL mode:

1. **SCK must be tied to GND** so the chip auto-generates its system clock from BCK.
   Floating SCK = silent DAC, the classic first-hour trap. Some purple GY-PCM5102
   breakouts ground it via a pad on the board; verify continuity, don't assume.
2. **Solder-jumper states** (typical breakout: FLT / DEMP / XSMT / FMT): FLT = normal,
   DEMP = off, **XSMT = high (un-mute — silent DAC trap #2)**, FMT = I2S (matches
   `I2S_COMM_FORMAT_STAND_I2S`). Names/positions vary per board — ⚠ VERIFY.
3. **Format compatibility is already correct:** 16-bit stereo standard I2S @ 48 kHz
   (BCK = 1.536 MHz) is squarely inside the PCM5102A's supported range; PLL mode locks
   from that BCK. No code change.
4. **Level into MIX IN:** PCM5102A full-scale is ~2.1 Vrms — *hotter* than consumer
   line level. The TD-02 mixes it with drum voices; a full-scale click will dominate or
   clip the mix. This is why the gain knob below is a bring-up requirement, not polish.
5. Wiring per `app_config.h` (BCLK 14 / LRCK 15 / DOUT 16) → DAC line-out → 3.5 mm →
   TD-02 MIX IN; Roland's internal metronome OFF (capture spec §4).

### I.3 Proposed changes (small, and all inside the existing layering)

**(a) `click_gain` config key** — `/config.txt`, 0–100 (%), default ~50. Applied in
`ClickRenderer::begin()` by integer-scaling the two pre-rendered voice buffers — zero
per-sample runtime cost, portable, covered in `test_click`. Rationale: the click level
that sits right in the TD-02's mix is a *setup* property (headphones, Tank-G chain,
taste); hardcoded amplitude is either inaudible under drums or clipping the mix.
Config-at-boot is sufficient for v1 (the console `click` command can grow an optional
gain argument later without schema impact — it writes nothing).

**(b) Move voice-mixing out of the click spinlock (small hardening).** `click_task`
currently calls `render_block()` inside `taskENTER_CRITICAL(&app.click_mux)` — a
core-0 interrupt-off window for the whole block render. Today that's tens of
microseconds and harmless; but it's the one place audio work sits inside a critical
section shared with the capture path's snapshot. Restructure to: lock → copy scheduler
state / advance edges → unlock → mix samples outside the lock. Turns "measured fine"
into "structurally fine" and shrinks the worst-case interrupt-off window on the capture
core. Low priority; do it during bring-up while watching the click counters anyway.

**(c) Record the legacy-driver decision as deliberate.** The legacy I2S API is the
*correct* choice while the project pins Arduino core 2.x / IDF 4.4 (the `usb_host` path
assumes the same core). If the toolchain ever moves to core 3.x, **only
`i2s_click_out.h` is replaced** (with the `i2s_std` channel API) — that's the port
system doing its job. No action now; one ADR row so it never looks like an oversight.

**Non-goals (explicit):** no mixing framework, no backing-track playback, no Bluetooth
audio (uncalibratable jitter — spec §4), no volume UI. `IAudioOut` stays a mono PCM
click sink. The audio subsystem's entire job is the click.

### I.4 Validation — Experiment 7 (roadmap style; closes ranked-unknown #8)

- **Purpose:** prove the DAC → MIX IN route practical and the schedule→sound offset
  *constant* (constancy is what matters — the constant lands in
  `calibration_offset_ms`; capture spec §4).
- **Measurements:** audible click at 60 / 120 / 208 BPM with correct accent pattern,
  10+ min each, no doubles/misses; scope (or audio-record) DOUT vs a render-time debug
  GPIO on a few edges → offset ≈ DMA depth and stable; click counters (late/underrun)
  zero while running `burst 5000 1000` + a sustained roll (Experiment 6 combined load);
  mix balance workable via `click_gain` through the real chain (TD-02 → Tank-G →
  headphones).
- **Success:** stable audible click under full concurrent load, constant pipeline
  offset, zero capture drops with click running.
- **Failure:** audible jitter/missed clicks under load (→ revisit task priorities/DMA
  depth), or offset drift (→ renderer timing model wrong — would be architectural).

---

## Part II — The device↔brain communication path

### II.1 What Phase 0 actually expects (studied before designing)

The brain's entire ingestion surface today:

- **`FileSource`** replays an append-only JSONL session log; `read_log` applies
  truncate-to-last-complete-line recovery; `reduce_session` folds declarations. The
  file format is **byte-frozen** by golden fixtures, and the firmware's
  `test_conformance` already proves this firmware's serializer reproduces those
  fixtures **byte-for-byte**.
- **`LiveMidiSource`** — *not* a device link. It is the laptop-phase **capture half**
  (capture spec §4A): it stamps raw MIDI on the host clock. It is the box's *peer*, not
  its consumer.
- The Phase 0 acceptance gate is `edrum replay` — byte-identical re-serialization.
- The spec names the coupling explicitly: *"They are joined by exactly one thing — the
  session file"* (capture spec §0); *"the box's files arrive through `FileSource`
  unchanged — that is the entire hardware migration story"* (`sources.py`, phase0-plan
  S5).

Two conclusions fall straight out:

**1. The faithful logical contract is "move the canonical bytes, unchanged."** Anything
that re-serializes, wraps records, or streams them in a new encoding creates a *second
dialect* of the corpus contract — a second thing to keep conformant, and a reopening of
the exact divergence risk the golden fixtures exist to close. The brain must never be
able to tell how a file arrived.

**2. Do not stream live MIDI to the brain.** Tempting (reuse `LiveMidiSource`), and
wrong twice over: it would put two clocks on one performance (the brain stamping
arrivals while the device owns the click — violates one-clock, spec §3 Rule 1), and it
would make both machines writers-of-record for the same session (two sources of truth).
The device is the capture half; the brain receives its **log**, never its raw MIDI.

### II.2 The logical contract: session-archive replication

**The device is the writer-of-record; the brain replicates its archive.** The unit of
exchange is the byte-exact session file under its canonical name
(`YYYYMMDDTHHMMSS_<id8>.jsonl` — already identical on both sides).

The acid test of transport-agnosticism: **pulling the SD card and copying files with a
card reader is the degenerate, zero-code implementation of this contract** (and works
today). Every real transport must produce results indistinguishable from sneakernet —
same names, same bytes, landing in `sessions/`. If a transport design can't pass that
test, it has leaked into the contract.

Contract semantics (v1):

- **Closed sessions only.** The currently-open session is not listed. Simplest honest
  semantics; no reader/writer races on a file being appended. (A live tail is a
  designed-for v2 extension — see II.8 — and per brain spec Phase 7 it must be *another
  thin consumer of the identical byte stream*, a tee at the `LogWriter` seam, never a
  second capture path.)
- **Read-only.** Protocol v1 cannot delete, rename, or write anything on the device
  (except wall-time injection, below). Files are never rewritten (brain spec §3);
  retention after a confirmed sync is a *user* act, deliberately out of the protocol.
- **Idempotent + resumable.** Sync converges: already-transferred files are skipped by
  name + size + checksum; a torn transfer resumes by offset. The brain **never
  overwrites** an existing local file — same name + same bytes = skip; same name +
  different bytes = refuse loudly (corpus protection).
- **Time rides along.** The handshake carries host wall time to the device — the same
  operation as console `settime`, now automatic on every sync. Every sync heals ADR-5
  session dating for free.
- **Versioned like the schema.** The handshake exchanges a protocol version and the
  device's `schema_version`; the client refuses a newer *major* on either — the same
  reader policy the corpus already obeys (brain spec §3).

### II.3 Protocol v1 sketch (the wire vocabulary)

Frame = `0x00` delimiter + COBS-encoded payload; payload = 1-byte channel, 1-byte type,
body, CRC32. Binary, self-delimiting, resynchronizable mid-stream. Channel byte
reserved: `0` = archive sync (this milestone), `1` = control plane (future — see II.8).

| Request (host→device) | Response (device→host) | Notes |
|---|---|---|
| `HELLO {proto_ver, host_time_iso}` | `HELLO_OK {proto_ver, schema_ver, fw_build, device_id}` | applies host time (≙ `settime`); majors checked both ways |
| `LIST` | `ITEM {name, size, crc32}`… `LIST_END` | closed sessions only |
| `FETCH {name, offset, len}` | `DATA {bytes}`… `EOF` | chunked ≤ 512 B/frame — bounds storage-task latency |
| `BYE` | `BYE_OK` | device returns to console mode |
| (any) | `ERR {code}` | unknown name, bad offset, busy, … |

Kilobyte-scale sessions (spec §6: drum MIDI is kilobytes) make even 115200 baud ≈
seconds per session. No compression, no windowing, no streaming ACKs — deliberately the
dumbest protocol that satisfies II.2, because the *contract*, not the transport, is the
deliverable.

### II.4 The transport abstraction: `hal::IByteLink` + framing in the core

```
edrum_core (pure, natively tested)          edrum_platform            src/
  frame.*        COBS + CRC32 + header        serial_link.h            wired in
  sync_service.* request→response state       (later: ble_link.h,      storage
                 machine over ISessionArchive  wifi_link.h, ...)        task loop
  hal/ports.h    IByteLink, ISessionArchive
```

- **`hal::IByteLink`** — a byte pipe: non-blocking `read(buf,cap) -> n`,
  `write(buf,len) -> bool`. A byte pipe is the least common denominator *every*
  candidate transport offers (UART, USB-CDC, TCP socket, BLE NUS-style characteristic
  pair), which is precisely what makes the port future-proof. Framing does **not** live
  in the adapters: COBS/CRC sits once in `edrum_core/frame.*`, so every future
  transport inherits identical, already-tested framing and the protocol engine is
  written once.
- **`SyncService`** (`edrum_core`) — a pure state machine: decoded frames in, response
  frames out; no I/O, no OS, no Arduino. Natively tested against a fake archive and
  scripted frames — the same discipline that just paid off for `ControlDispatcher`
  (capture spec §12: control routing became natively coverable instead of
  hardware-only).
- **`hal::ISessionArchive`** — the read-side storage port: `list()`, `stat(name)`,
  `read(name, offset, buf, len)`. **Deliberately a separate port from `IStorage`**:
  the writer's single-open-file, append-only, exclusive-create model is load-bearing
  for crash-survivability, and quietly widening it into a general filesystem API would
  erode that guarantee. Reading back finished sessions is a different capability with
  different concurrency needs; it gets its own narrow port, implemented by a sibling
  SdFat adapter in `edrum_platform`.

**Concurrency (the one real integration decision):** the SD card and its SPI bus are
owned by the storage task. Rather than adding a mutex around SdFat (a new
cross-task-blocking edge on the write path), **the sync service runs inside the storage
task's loop**: each iteration drains the ring first (capture always wins), then services
at most one bounded link chunk (≤ 512 B read). Single SD owner by construction — the
same shape as the "single ring producer" discipline. A transfer physically cannot smear
a timestamp (wrong side of the ring, wrong core) and cannot starve the log (ring drain
is unconditionally first; the existing stall counters instrument any interaction).

### II.5 First concrete transport: serial, on the UART bridge port

The validation transport is **framed serial over the already-wired CP210x UART port**
(the console port). Why this and not BLE/Wi-Fi first:

- **Zero new hardware and zero new radio stack** — ADR-4 already validated this port
  working while the kit stays hosted on the OTG port.
- **It's the least interesting transport, which is the point.** The milestone's
  deliverable is the seam (`IByteLink` + `SyncService` + `ISessionArchive` + the brain
  client). Serial proves all of it while pinning nothing; Wi-Fi/BLE become sibling
  adapters plus their own roadmap experiments (RF vs. capture-core contention, power)
  when a cable-free workflow earns them.
- **Console coexistence, v1 rule:** modal, not multiplexed. A `HELLO` frame (or `sync`
  console command) switches the line to framed mode; console text output is suspended
  for the duration; `BYE` or an idle timeout restores it. This avoids interleaving
  corruption (multiple tasks print to `Serial`) without inventing a mux protocol.
  Frames are self-delimiting, so a v2 could multiplex console-as-a-channel later if
  ever wanted.

### II.6 Brain side: `io/devlink.py` + `edrum sync`

- `edrum/io/devlink.py` — the protocol client over a mirrored byte-link seam (pyserial
  under a new `[device]` extra, exactly parallel to `[live]`'s rtmidi). A TCP byte-link
  is a drop-in later; the client code doesn't change.
- CLI: `edrum sync --port COM5 [--out sessions]` — handshake (+ time injection), list,
  fetch missing/partial files, then verify each landed file three ways: transfer CRC,
  `read_log` parses clean, and the existing **byte-identity replay check** — the Phase 0
  gate itself, now running against device-produced files. Refuse-to-clobber per II.2.
- **`engine/` is untouched. `Source` is untouched.** The transport lands files; they
  arrive through `FileSource` unchanged — the sync path is an `io/`-layer concern, and
  "the box's files arrive through `FileSource` unchanged" stays literally true.

### II.7 What this milestone deliberately does *not* build (but frames for)

- **Live session tail (v2):** a `TAIL` frame type on channel 0 — a byte-level tee at
  the `LogWriter` append seam, serving the same canonical bytes as they hit SD. This is
  how a future live view stays "another thin consumer of the identical event stream"
  (brain spec Phase 7) instead of a parallel capture path. Reserved, not built.
- **Remote control plane (channel 1):** the vocabulary already exists and is already
  transport-agnostic — `ControlMsg` (capture spec §5/§12). A future app/BLE controller
  sends ControlMsg-shaped frames (plus the metronome knob, which is deliberately *not*
  a declaration) into the existing control queue → `ControlDispatcher` path. The
  framing's channel byte is the only provision v1 makes.
- **Device-initiated push / discovery / pairing.** v1 is host-initiated pull: the
  device stays dumb, passive, and deterministic (the two-machine split's whole point).
  Push belongs with a network transport, if ever.
- **Deletion/retention protocol.** User act, not protocol semantics.

---

## Part III — Refactor assessment: none required

The question was asked directly, so answering it directly: **the current design should
not be refactored before adding these capabilities.** Every seam this milestone needs
already exists and is already proven in the codebase's own style:

- ports + adapters (`IClock`/`IStorage`/`IAudioOut` precedent for `IByteLink`/`ISessionArchive`),
- pure-core state machines with native tests (`ControlDispatcher`, `SessionController`,
  `LogWriter` precedent for `SyncService`),
- single-owner concurrency via queues/rings (precedent for SD ownership in II.4),
- a source-agnostic vocabulary for control (`ControlMsg`) that the future control
  channel reuses as-is.

The additions are: two ports, two pure core modules (`frame`, `sync_service`), one
platform adapter (`serial_link`) plus one archive adapter (`sd_archive`), storage-task
wiring, and one brain `io/` module + CLI command. The only pre-existing wrinkle worth
touching is the render-inside-spinlock trim (I.3b), and it is optional hardening, not a
blocker.

---

## Part IV — Build order & the milestone gate

1. **I2S/PCM5102 bring-up** (I.2, I.3a) → Experiment 7 sign-off. *Independent of
   everything below; start immediately.*
2. **Core comms, native-first:** `frame.*` → `ISessionArchive` + fake → `SyncService`
   (+ `test_frame`, `test_sync_service` — including torn-frame resync, resume-at-offset,
   version refusal, busy/unknown-name errors).
3. **Platform + wiring:** `sd_archive.h`, `serial_link.h`, storage-task integration,
   console modal switch.
4. **Brain client:** `devlink.py`, `edrum sync`, `[device]` extra, tests against a
   scripted fake link (no hardware needed — same trick as `FakeSource`).
5. **The end-to-end gate (the milestone's definition of done):** on the physical box —
   click running via PCM5102 through MIX IN, play a real session including a
   gesture-declared grid span, let it idle-close; then `edrum sync --port COMx`; then
   `edrum replay` **passes byte-identity** on the transferred file and
   `edrum dump --normalized` shows lanes. That single run exercises audio, capture,
   declarations, storage, transfer, and the brain contract in one loop — the first true
   end-to-end integration.
6. Housekeeping: flip ADR-1/3/6 to Validated with the measured numbers now in hand
   (roadmap statuses predate the recent successful runs).

## Part V — Spec/roadmap edits on approval

- Capture spec: new **§13 Device↔Brain link** (logical contract II.2, ports II.4,
  serial-first II.5, the sneakernet test); §4 gains the PCM5102 bring-up notes (I.2)
  and `click_gain`; §10 register rows for transport + DAC.
- Roadmap: **Experiment 7** (I.4) and **Experiment 8** (link under load: sync during
  `burst` + roll; success = zero ring drops, zero stall-budget violations, CRC-clean
  transfer); **ADR-7** (transport-agnostic archive link, serial first) and the
  legacy-I2S-driver note (I.3c).
- Brain spec: one line in §2's `io/` list (`devlink` — device archive client; engine
  untouched). The §3 contract needs **no change** — that's the design working.

## Decisions — RESOLVED 2026-07-12 (all implemented)

| # | Decision | Resolution |
|---|---|---|
| 1 | Transport arbitration (console vs sync) | **Modal switch via the `sync` console command**: interactive console suspended, line handed exclusively to `hal::IByteLink`; clean BYE or 10 s idle timeout reinstates the console. Task prints suppressed during framed mode. |
| 2 | `click_gain` scope | **Firmware-local only** (config → composition root → renderer init). Never brain profiles, never kit-profile data — a future DAC swap re-tunes gain strictly inside firmware configuration. |
| 3 | Framing/error detection | **CRC-16 (CCITT-FALSE) + COBS** over `IByteLink` — right speed/robustness balance for a local serial wire, without CRC-32 overhead. Chainable for bounded-chunk file checksums. |
| 4 | Concurrency hardening | **State changes (edge decisions, scheduler advance) inside `click_mux`; mixing/attenuation outside**, into the click task's dedicated block buffer — core-0 interrupt-off windows stay sub-millisecond by construction (`plan_block`/`mix_block` split). |
| 5 | Storage fault tolerance | **Strict fail-soft**: card missing/full/corrupt latches `LogWriter.failed()`; records discard gracefully (counted, ring always drained — no queue overflow); capture + click keep running as a standalone unit; a host sync in this state gets an explicit `ERR StorageFailed` on the wire, never a freeze. |
