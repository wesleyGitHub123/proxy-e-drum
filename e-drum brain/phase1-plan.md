# Phase 1 Implementation Plan — PROPOSED (awaiting approval)

*Drafted 2026-07-07. This document is the proposed implementation contract for the next
brain milestone after Phase 0, at the same rigor as `phase0-plan.md` (which drove a
near-one-shot implementation). The product contract remains `edrum-brain-spec.md`; this
plan executes spec §9 Phase 1 **with a deliberate scope correction** (see "Milestone
determination"). Nothing here changes the frozen session-file schema —
`schema_version` stays 1 and every Phase 0 golden fixture must remain byte-identical.*

---

## 1. Milestone determination (and where this challenges the spec)

### The dependency the spec's phase list hides

Spec §9 Phase 1 is "Layer A: reference-free floor" and defers "Declarations & control
surface" to Phase 3. But Layer A's own validation gate — *record a metronomic take and a
degraded take; metrics must rank them* — is impossible under that split:

- Layer A grades `events × grid_track` (§1A).
- A grid segment exists only where a grade span was **declared** against a **sounding
  click** (§5, declaration mechanics).
- Phase 0 shipped the declaration *schema* and the *fold*, but no producer: there is no
  click (the host is not yet a tempo authority) and no control input that writes
  `grid_start`/`grid_end` during capture.

So as literally phased, Phase 1 could only ever be validated on synthetic fixtures, and
its adjudication obligation ("the rush/drag number matches felt sense", §10) would be
unmeetable until Phase 3. The original decomposition is not optimal here.

### The correction: split Phase 3, not merge it

Phase 3 contains two separable things:

1. **Declaration plumbing** — a control input that writes declarations mid-session,
   snapshotting the running click. Cheap, laptop-phase, keyboard-driven.
2. **The gesture grammar** — recognizing pad-hit gestures as that control input, with a
   ≈0 false-trigger rate, plus trigger-hit exclusion. This is the actually-hard part.

Item 1 moves into this milestone; item 2 stays in Phase 3 (which shrinks to
"gesture grammar as an *alternate producer of the same commands*"). This is consistent
with spec §8's own note: *"in the laptop phase, gesture detection and dating live in the
brain/host"* — a keyboard is the laptop-phase control-input channel, and DECISION 9c
already anticipated the migration.

### The milestone

> **Phase 1 — the graded-practice loop.** Everything needed to go from `edrum record`
> to honest timing numbers for a declared graded span, end to end, on the laptop:
> the pure Layer-A engine, the host click (tempo authority on the one clock), the
> keyboard declaration surface, and the `analyze` CLI.

**Why these components belong together:** each half is unvalidatable alone. The Layer-A
engine without the click path can only be exercised on synthetic data (no adjudication);
the click/declaration path without the grader produces grid segments nothing consumes
(no user value, no validation of the geometry). Together they close the loop the entire
product is built around — and they start the clock on the **Layer-D corpus**: every real
practice session recorded after this milestone ships is longitudinal validation data for
Phase 2. That corpus is calendar-gated (≥5 sessions across weeks, §9 Phase 2), so
shipping the loop early is what makes Phase 2 validatable on schedule.

### Reordering / splitting / deferral summary

| Item | Disposition | Rationale |
|---|---|---|
| Layer A engine | **This milestone** | Spec Phase 1 core; first real consumer of the frozen contract |
| Host click (tempo authority) | **This milestone** | Spec Phase 1 "asks of capture"; required by declarations |
| Declaration plumbing (keyboard) | **Pulled forward from Phase 3** | Without it Layer A cannot be validated on real playing |
| Gesture grammar + trigger-hit exclusion | Stays Phase 3 | The hard, separable part; keyboard makes it non-blocking |
| Layer D | **Next milestone (Phase 2)**, unchanged order A→D | Its engine could be built now, but its validation is calendar-gated; bundling would block this milestone on weeks of elapsed time. Result objects here are designed as D's time-series atoms (see audit) |
| Calibration *routine* (auto-measurement) | Mostly deferred (see decision D2) | The schema field exists; manual `--calibration` works today; the honest auto-measurement needs design. Engine-side handling (subtract + flag uncalibrated) IS in scope |
| Audio replay (§4B) | Deferred; **spec gap: it is slotted in no phase** — recommend explicitly slotting after Phase 2 | Must-have overall, but not in the grading loop's critical path; needs sample assets |
| BPM bucketing (spec Phase 1 open decision) | **Dissolved / moved to Phase 2** | Declared tempo is exact, so Layer A groups by exact `(bpm, subdiv)` — no boundaries needed. Bucketing only matters for cross-session trend (D) and inferred tempo (B) |
| numpy/scipy (phase0-plan dep table said "Phase 1") | **Challenged: not adopted** | Layer A is means/stds/histograms over ≤ thousands of ints; stdlib suffices. Keeping the engine dependency-free preserves the on-device-port story (§2). numpy earns its keep at Layer B |
| MIDI export, derived-metric cache, live drift indicator | Deferred (Tier 3, unchanged) | Spec §12 |

### Spec corrections to apply on approval (living-document edits)

1. §9 Phase 1 "Asks of capture: … **BPM in meta**" is stale pre-§1A language — BPM lives
   in grid segments, never meta. Replace with "click as tempo authority + a grade-span
   declaration input (laptop phase: keyboard); grid params travel in declarations."
2. §9 Phase 3 rescope: declaration plumbing → Phase 1; Phase 3 = gesture grammar as an
   alternate command producer + the additive trigger-hit-span field + engine-side
   trigger exclusion.
3. §9 Phase 1 decision "BPM bucket boundaries" → move to Phase 2.
4. Slot §4B audio replay explicitly into the phase list (recommended: after Phase 2).
5. Decision register: add the semantic freezes below (deviation sign convention, lattice
   definition, `subdiv` meaning, `downbeat_t` beat-anchoring).
6. Note for Layer C: declarations carry no bar-length/time-signature — folding "onto one
   bar" will need either periodicity inference or an additive field (decision D6).

---

## 2. Scope

**In:** `engine/grid.py` (pure lattice), `engine/analyze.py` (Layer A metrics + result
model), `io/click.py` (click engine on the shared clock), `io/control.py` (command →
declaration protocol), graded-capture record loop + keymap, `edrum analyze` command,
golden-value metric fixtures, live validation gate.

**Out (explicitly):** any schema change (none is needed — this is a feature of the
milestone, not an accident); gestures; Layer B/C/D computation; audio replay; MIDI
export; plotting (optional stretch, `[plot]` extra, CLI-side only); metric persistence
of any kind.

## 3. Dependencies

| Dependency | Why | This milestone? |
|---|---|---|
| Phase 0 (`edrum` package, 119 tests, frozen fixtures) | everything | Yes — unchanged |
| `sounddevice` (PortAudio) | click audio callback | Yes — new optional extra `[audio]`; engine and `analyze` must work without it |
| `msvcrt` (stdlib, Windows) | keyboard polling in `record` | Yes — POSIX fallback deferred, noted in code |
| numpy/scipy | — | **No** (see challenge above; Layer B) |
| `matplotlib` | optional `analyze --plot` stretch | Optional `[plot]` extra only; never imported by engine |

## 4. Layout (additions only)

```
edrum/
  engine/   grid.py      analyze.py          (pure — never imports io/cli)
  io/       click.py     control.py          (+ a graded-capture loop in capture.py)
  cli/      main.py                          (+ analyze command, record keymap)
tests/      test_grid.py test_analyze.py test_click.py test_control.py
            test_e2e_graded.py
            fixtures/graded_synthetic.jsonl  (+ expected-metrics golden values in test code)
```

Rule unchanged and re-asserted: `io/` may import `engine/`; `engine/` never imports
`io/` or `cli/`. `engine/analyze.py` takes `Session` / `KitProfile` **objects**, never
paths; conversion of results to dicts is a pure engine function; printing is CLI-only.

---

## 5. Semantic micro-decisions (proposed FINAL — these freeze *meanings*, the way Phase 0's froze bytes)

Numbering continues from phase0-plan's 1–6. None of these touch serialized bytes; they
freeze the semantics that golden-*value* tests will guard forever.

7. **Deviation sign & calibration order.** For an event at `t` graded by a segment:
   `t_corrected = t − calibration_offset_ms` (meta value; `null` → 0 **and** the result
   is flagged `calibrated=False`). Assignment and deviation both use `t_corrected`:
   `deviation_ms = t_corrected − nearest_gridline(t_corrected)`. **Positive = late
   (drag), negative = early (rush).** Rationale for the order: calibration is applied
   *before* nearest-line assignment, because a large systemic offset would otherwise
   misassign events near midpoints. `calibration_offset_ms` is defined as the total
   systemic lateness of a stamped hit relative to the heard click, `L_audio + L_midi`
   (a perfectly-heard-on-click hit stamps at gridline + L_audio + L_midi; subtracting
   the sum yields 0). This pins the meaning of the existing meta field.
8. **The lattice.** `tick_ms = 60000 / bpm / subdiv` (float; §6 formula). `subdiv` =
   ticks **per beat**: 1=quarters, 2=8ths, 3=8th-triplets, 4=16ths. Gridlines =
   `downbeat_t + k·tick_ms` for **all integers k** (the lattice is unbounded; the
   segment bounds which *events* are graded, not which lattice points exist — an event
   just inside a segment may grade against a line marginally outside it). Beats =
   `downbeat_t + j·(60000/bpm)`. `downbeat_t` is **beat-anchored**: it must be the time
   of an actually-sounded *beat* tick (declarations snapshot the most recent beat tick
   ≤ the declaration's `t`). Beat anchoring is load-bearing for Layer C's beat-relative
   folding and for click accents; a subdivision-anchored value would silently rotate
   the beat phase.
9. **Segment membership.** An event is graded by segment `[start_t, end_t]` inclusive;
   where the param-change rule makes two segments share a boundary `t`, the **later
   segment wins** (the change takes effect at the change point).
10. **Statistics.** Mean requires n≥1; std is the sample std (ddof=1), requires n≥2,
    else `None` — never 0.0 (a fake zero is a confident claim the data can't support).
    No outlier rejection in v1 (honest floor; trigger-hit exclusion arrives with
    Phase 3). **Assignment ambiguity is surfaced, not hidden:** events with
    `|deviation| > 0.35·tick_ms` are counted (`n_ambiguous`) and still included —
    a high ambiguous fraction is the honest signal that nearest-line grading is
    breaking down on this material (Layer B's cue). Threshold is a named engine
    constant; revisit at Layer B (decision D3).
11. **Results are derived views — deliberately unfrozen.** Analysis results are never
    written to session files (§1A invariant 2) and their dict/JSON shape may evolve
    freely between engine versions; there is no golden-*byte* pressure on them. What
    IS frozen: numeric behavior, guarded by golden-**value** tests (fixed fixtures →
    expected metrics asserted to 1e-6 abs; summation in `t`-order for determinism).
    Every result object carries provenance: `session_id`, `analysis_version` (new
    engine constant `ANALYSIS_VERSION = 1`), `(kit_profile_id, profile_version)` when
    a profile was used, `calibration` status, and `n` for every aggregate. This is
    Layer D's attributability requirement (§4.2, §6) paid forward at zero cost.
12. **One lattice truth.** The click renderer computes tick times **only** via
    `engine/grid.py` on the session clock. It never keeps a free-running sample-counter
    schedule: the audio device's crystal drifts from `perf_counter` (tens of ppm ⇒
    tens of ms over a long session), so each audio callback **re-anchors** the buffer's
    session-time and places ticks sample-accurately within it. If the click and the
    grader ever computed gridlines independently, the grade would compare against a
    grid that was never heard — this rule is the click-side corollary of the one-clock
    rule. Sounding the click writes nothing (§5).
13. **Declaration mechanics at capture (v1 rules, resolving the spec's either/or):**
    grade-span/enroll declarations with no click sounding are **rejected with a
    message** (nothing to snapshot, §5). A BPM/subdiv change while a span is open
    **closes the segment at the change point and opens a new one** with the new
    snapshot (matches the fold's param-change rule — capture and fold stay symmetric;
    the spec's "disallow" alternative is rejected). Stopping the click while a span is
    open closes the span. Session stop writes explicit `grid_end`/`enroll_end` for any
    open span **before** `session_end` — the fold's auto-close remains a crash safety
    net, never the normal path. Bookmarks are allowed anytime (no click required).
    A param change while the click runs re-anchors the lattice (`downbeat_t` = the
    change point, where the first new-tempo beat sounds).
14. **Record-loop ordering protocol (single consumer, no locks).** One consumer thread
    owns the writer and does everything: drain the source queue (50 ms poll), poll the
    control input, process commands. Declarations are stamped on the consumer thread
    *after* a full queue drain. Because the MIDI callback stamps `t` before enqueueing,
    a µs-scale race exists where an earlier-stamped event arrives after a later-stamped
    declaration; a raw write would trip the writer's hard monotonic assert and **kill a
    live session** (unacceptable even at low probability). Resolution: a stamped
    declaration is **staged**, and flushed when either (a) the next drained record has
    `t ≥` the staged `t` (declaration written first, preserving t-order), or (b) 20 ms
    of wall time passes — whichever comes first; a drained record with `t <` the staged
    `t` is written before it. Event/`ctrl` stamps are **never modified** (they are
    measurements); declaration stamps are never modified either — only their *write* is
    briefly deferred. The writer's hard assert stays. (~15 lines, fully unit-testable
    with `FakeClock` and scripted inversions.)
15. **Metric groupings.** Deviations: overall per segment + per raw note (+ per
    `(instrument, articulation)` lane when a profile is supplied). Velocity: per raw
    note primary (+ lane aggregation via profile — raw-note stats are always valid;
    lane stats are profile-dependent views, consistent with §4.2). IOI: pooled over the
    span + per note; the engine returns raw deviation/IOI lists alongside summaries —
    histogram binning is presentation and belongs to callers (CLI now, Layer B later).

---

## 6. Subsystems

### S9 — Grid geometry (`engine/grid.py`)

- **Purpose:** the single source of lattice truth (micro-decisions 8, 9, 12): where
  gridlines and beats are, for *both* the grader (past) and the click (future).
- **Responsibilities:** `tick_ms`/`beat_ms`; nearest-gridline assignment; enumerate
  gridlines/beats in a half-open time range (for the click renderer and plotting);
  segment membership including the shared-boundary rule.
- **Explicit non-responsibilities:** no metrics, no calibration handling (caller
  corrects `t` first — S10), no audio, no I/O, no state.
- **Inputs:** grid params (`bpm`, `subdiv`, `downbeat_t`) or `GridSegment`s; times in ms.
- **Outputs:** floats/ints and small value tuples (gridline time, index, deviation).
- **Public interfaces:** `tick_ms(bpm, subdiv) -> float`; `beat_ms(bpm) -> float`;
  `nearest_gridline(t, bpm, subdiv, downbeat_t) -> tuple[float, int]` (grid time,
  lattice index k); `gridlines_in(start_t, end_t, bpm, subdiv, downbeat_t) ->
  list[float]`; `beats_in(...) -> list[float]`; `segment_for(t, grid_track) ->
  GridSegment | None`.
- **Internal data flow:** pure arithmetic; no module state.
- **Dependencies:** `engine/session.py` types only.
- **Acceptance criteria:** property tests — every deviation ∈ (−tick/2, tick/2];
  lattice invariance under `downbeat_t ± k·tick`; `gridlines_in` matches
  `nearest_gridline` self-consistently; boundary rule cases; float determinism on the
  fixture inputs.
- **Extension points:** Layer B calls `nearest_gridline` against *inferred* params
  unchanged; Layer C uses `beats_in`/beat indices for beat-relative folding; the
  firmware click implements this module's math as its reference (conformance by
  golden values).
- **Migration risks:** none to the corpus (computes, never stores). The risk is
  *semantic* drift — a second lattice implementation appearing anywhere (click DSP,
  plotting, Layer B). Mitigation: micro-decision 12 + code review rule: no
  `60000 /` outside this module.
- **WHY NOW:** everything downstream in this milestone (grading and click alike)
  is this arithmetic; building it once, first, is what makes them provably agree.

### S10 — Layer A analysis (`engine/analyze.py`)

- **Purpose:** spec §6 Layer A — the reference-free floor: deviations, rush/drag,
  consistency, velocity stats, IOI; the result model every later layer builds on.
- **Responsibilities:** two honest tiers — **grid-free** stats (velocity, IOI) valid
  over any span, and **grid-bound** stats (deviations and their aggregates) computed
  only inside grid segments; calibration handling per micro-decision 7; claim
  bounding (n everywhere; `None` + reason instead of fake numbers; `n_ambiguous`;
  `calibrated` flag; explicit empty-grid refusal: "no grid segments — nothing is
  gradeable" as a structured result, not zeros); provenance stamping (micro-decision
  11); result→dict conversion (pure).
- **Explicit non-responsibilities:** no storage of any result, ever (§1A invariant 2);
  no file/path handling; no printing; no subdivision inference (Layer B); no
  cross-session anything (Layer D); no trigger-hit exclusion yet (Phase 3 — but see
  extension point); no histogram binning.
- **Inputs:** `Session` (from `reduce_session`), optional `KitProfile` object,
  optional explicit `segments` list (defaults to `session.grid_track`).
- **Outputs:** frozen dataclasses: `SessionAnalysis` → per-segment `SegmentGrade`
  (raw deviation list, mean/std, per-note and per-lane breakdowns, `n`, `n_graded`,
  `n_ambiguous`, grid params) + session-wide `SpanStats` (velocity per note/lane,
  IOI pooled + per note) + provenance header.
- **Public interfaces:** `analyze_session(session, *, profile=None, segments=None) ->
  SessionAnalysis`; `grade_segment(events, segment, calibration_offset_ms) ->
  SegmentGrade`; `span_stats(events, *, profile=None) -> SpanStats`;
  `analysis_to_dict(SessionAnalysis) -> dict`; `ANALYSIS_VERSION`.
- **Internal data flow:** filter events to segment (S9 membership) → correct `t`
  (calibration) → assign + deviate (S9) → aggregate in `t`-order → wrap with
  provenance. Grid-free tier never touches S9.
- **Dependencies:** S9, `engine/session.py`, `engine/records.py`,
  `engine/normalize.py` + `profiles.py` (lane views). Stdlib only (`statistics`).
- **Acceptance criteria:** synthetic analytic fixture — events placed at exact
  gridlines + known offsets → every metric equals its hand-computed value (1e-6);
  grid-empty session → grid-free stats present, grid-bound tier explicitly refuses;
  `calibration_offset_ms: null` → `calibrated=False` flagged on rush/drag;
  n=1 segment → std `None`; per-lane views change when the profile is re-pointed
  (the SSD property, observed at the metrics level).
- **Extension points:** `segments=` parameter is Layer B's entry (recompute against
  inferred grids and tag confidence — B wraps, never forks, this module); an optional
  event-exclusion predicate parameter is reserved (documented, not built) for
  Phase 3 trigger-hit exclusion; `SessionAnalysis` dataclasses are Layer D's
  time-series atoms.
- **Migration risks:** none to the corpus. Risk is over-freezing the result shape —
  averted by micro-decision 11 (shapes evolve; values are golden).
- **WHY NOW:** this is the first *consumer* of the contract Phase 0 froze — the
  cheapest possible probe of whether `grid_track`'s fields, `downbeat_t` semantics,
  and the fold are actually sufficient, before new capture machinery is built on
  top of them. It is also the milestone's user value.

### S11 — Click engine (`io/click.py`)

- **Purpose:** the host as tempo authority (capture spec §4A; brain spec §8) — the
  audible click, scheduled on the *same* `Clock` instance that stamps MIDI (the
  one-clock rule, honored by construction exactly as Phase 0's S4 anticipated).
- **Responsibilities:** click state (running/stopped, `bpm`, `subdiv`, lattice anchor);
  start/stop/param-change with re-anchoring (micro-decision 13); `snapshot()` for
  declarations; audio rendering via `sounddevice` callback — per callback, map the
  output buffer's start to session-ms (perf_counter sampled in the callback + the
  stream's reported output latency), ask **S9** for ticks/beats in the buffer's window,
  synthesize (accented beat tick, quieter subdivision tick — short synthesized bursts,
  no sample assets; §4B's sample playback is a different, later feature); expose the
  stream's reported output latency (input to calibration, D2).
- **Explicit non-responsibilities:** **writes nothing** (sounding the click writes
  nothing — §5's crucial separation; enforced structurally: this module never sees the
  writer); no lattice math of its own (micro-decision 12); no bar/time-signature
  accents (no such concept in the schema — decision D6); no MIDI.
- **Inputs:** the shared `Clock`; user commands (from S12).
- **Outputs:** sound; `ClickSnapshot(bpm, subdiv, downbeat_t) | None`.
- **Public interfaces:** `ClickEngine(clock, *, samplerate=48000)` with
  `start(bpm, subdiv)`, `set_bpm(bpm)`, `set_subdiv(n)`, `stop()`,
  `snapshot() -> ClickSnapshot | None`, `reported_output_latency_ms`, `close()`.
  One concrete class — **no protocol/ABC** (the only other implementation ever will be
  firmware, not Python; an interface with one implementer is decoration).
- **Internal data flow:** audio thread reads an immutable params struct swapped
  atomically by control-thread calls; callback: session-time anchor → S9 tick
  enumeration → sample placement. Param changes take effect at the next callback
  boundary (≤ one buffer, ~10–20 ms — inaudible for a human tempo change).
- **Dependencies:** S9, `io/clock.py`, `sounddevice` (`[audio]` extra; import inside
  the class so file-analysis workflows never need PortAudio).
- **Acceptance criteria:** pure schedule tests (tick/beat times for the buffer windows
  are exact per S9); manual hardware gate: click audibly steady at 60/90/120 BPM
  subdiv 1–4 for ≥5 min; **latency spike** (build-order step 4): loopback-record the
  click once in a DAW, verify inter-tick spacing jitter ≲ 2 ms and measure the
  constant offset (documented; feeds D2). Constant offset is absorbed by calibration;
  *jitter* is the real enemy and the go/no-go number.
- **Extension points:** the pure schedule (S9) is the reference the firmware click
  must match; the calibration routine (D2) consumes `reported_output_latency_ms`.
- **Migration risks:** none to the corpus (writes nothing). Platform risk: WASAPI
  latency/jitter — isolated behind the pure schedule and probed by the spike before
  any UX work depends on it.
- **WHY NOW:** declarations *require* a sounding click (§5); no click ⇒ no grid ⇒
  no real-world Layer A validation. This is also the first exercise of Phase 0's
  claim that S4's shared-instance design satisfies the one-clock rule.

### S12 — Control surface & graded capture loop (`io/control.py`, `io/capture.py`, `cli` record integration)

- **Purpose:** the laptop-phase control-input channel (§8): user commands →
  declaration records, with the click as the thing declarations snapshot.
- **Responsibilities:** the **command protocol**, input-agnostic and headless-testable:
  `Command` values (`ToggleClick`, `SetBpm`, `CycleSubdiv`, `GradeToggle`,
  `EnrollToggle(profile_ref)`, `Bookmark`, `Stop`) → validated declarations per
  micro-decision 13 (reject without click; param-change split; close-on-click-stop;
  explicit ends at session stop); the staging/ordering protocol (micro-decision 14);
  the single-consumer graded-capture loop (`run_graded_capture(source, writer, click,
  control_input, clock)`) interleaving queue-drain + control-poll; CLI: `msvcrt`
  keyboard poller mapped to commands (fixed tiny keymap: `c` click, `[`/`]` BPM,
  `s` subdiv, `g` grade span, `e` enroll, `m` bookmark, `q`/Ctrl+C stop) and status
  echo. Enrollment and bookmarks ride along because their mechanics are identical to
  grade spans (same snapshot rule, same staging) — marginal cost ≈ zero, and every
  enrolled span recorded from now on is Layer C validation corpus.
- **Explicit non-responsibilities:** no gesture recognition (Phase 3 = an alternate
  producer of these same `Command`s — that is the entire Phase 3 seam); no analysis;
  no normalization; no writer-internals knowledge beyond `append`; no config/keymap
  customization (scope discipline).
- **Inputs:** `Source` (unchanged Phase 0 seam), `SessionWriter` (unchanged),
  `ClickEngine`, a polled `control_input: Callable[[], Command | None]`, the shared
  `Clock`.
- **Outputs:** the session file — now with real declaration lines; console status.
- **Public interfaces:** `Command` union; `ControlState` (open-span bookkeeping);
  `run_graded_capture(...) -> count`; CLI keymap (cli-side).
- **Internal data flow:** one consumer thread: drain source queue → flush/stage per
  micro-decision 14 → poll control input → validate command against click/span state →
  stamp + stage declaration → repeat. MIDI callback thread and audio thread never
  touch the writer.
- **Dependencies:** S11, Phase 0's S2/S4/S5/S7, `engine/records.py`.
- **Acceptance criteria:** headless scripted tests (FakeClock + FakeSource + scripted
  command sequences): warmup-with-click-and-no-declaration → **zero** grid lines in
  the file (the §1A integrity invariant, now enforced at capture, mirroring the
  Phase 0 fold fixture); declaration without click → rejected, file untouched; BPM
  change mid-span → two segments splitting at the change; stop with open span →
  explicit ends before `session_end`; staging race test (scripted stamp inversion) →
  file passes the writer's monotonic assert and `edrum replay` byte-identity;
  kill-mid-graded-span → recovery + fold auto-close (e2e).
- **Extension points:** Phase 3 plugs a gesture recognizer in as another
  `control_input`; the box migrates this module's *rules* (micro-decision 13) into
  firmware, and this module's scripted acceptance runs become firmware conformance
  cases (files must fold identically).
- **Migration risks:** the declaration records it writes are corpus-permanent — but
  their schema was frozen in Phase 0 (S1) and is untouched. The trigger-hit-span
  field gestures will need is **additive** under the ignore-unknown-fields policy
  (verified assumption, see audit). Behavioral risk: the ordering protocol — covered
  by the race test.
- **WHY NOW:** it is the only missing producer between the frozen schema and real
  graded data; every week it doesn't exist is a week of practice history without
  grids (unrecoverable — capture-time information, §4.2 test).

### S13 — Analysis CLI (`cli/main.py` — `edrum analyze`)

- **Purpose:** the thin caller for S10 (§2): make the numbers visible.
- **Responsibilities:** `edrum analyze FILE [--json] [--profiles-dir DIR] [--kit ID]`
  — load session (Phase 0 io), resolve profile from meta (`--kit` override = the
  re-pointable-profile guardrail §4.2, exercised at the CLI for the first time),
  call `analyze_session`, print per-segment table + session-wide stats + warnings,
  or emit `analysis_to_dict` JSON. Optional stretch: `--plot` (matplotlib, `[plot]`
  extra) drawing events vs gridlines — adjudication aid only.
- **Explicit non-responsibilities:** zero logic (any conditional more interesting
  than arg handling belongs in engine/io); no persistence of results anywhere.
- **Inputs:** file path, flags. **Outputs:** text/JSON to stdout; exit code.
- **Public interfaces:** the command itself.
- **Internal data flow:** `read_log` → `reduce_session` → `load_profile` →
  `analyze_session` → print.
- **Dependencies:** S10, Phase 0 io/cli plumbing.
- **Acceptance criteria:** golden-value CLI test on the synthetic fixture (stable
  numbers in output); `--json` round-trips through `json.loads`; grid-empty file →
  clear "nothing gradeable" message, exit 0 (absence of grid is truth, not error).
- **Extension points:** the future app calls `analyze_session`/`analysis_to_dict`
  directly — the `--json` shape is a preview, not a contract (micro-decision 11).
- **Migration risks:** none.
- **WHY NOW:** the loop isn't closed until a drummer can *see* the numbers; also the
  vehicle for the milestone's validation gates.

### Fixtures & golden values (`tests/fixtures/graded_synthetic.jsonl` + value tables)

Built with FakeClock/FakeSource through the **real** pipeline (as in Phase 0 Slice 1):
a synthetic graded session whose deviations are constructed analytically (hits at
gridline + {0, +10, −10, …} ms around a known lattice, plus a free-play span, plus an
uncalibrated + a calibrated variant). Expected metrics are hand-computed and asserted
to 1e-6. Per spec §10, this validation gate becomes the permanent regression suite —
and the future firmware's graded-session conformance case. Phase 0 fixtures are
untouched and must stay byte-identical (the milestone's zero-schema-change claim,
tested).

---

## 7. Dependency graph & build order

```
Phase 0 (frozen): S1 records ── S2 logfile ── S3 fold ── S4 clock ── S5 sources ── S6 profiles ── S7 capture
                                                                                        │
S9 grid (pure) ──┬─→ S10 analyze (pure) ──→ S13 `analyze` CLI ──→ golden-value fixtures │
                 │         ↑ S3, S6                                                     │
                 └─→ S11 click (io/audio) ──→ S12 control + graded loop ←───────────────┘
                           ↑ S4 clock                ↑ S2 writer, S5 sources
```

**Build order: S9 → S10(minimal) → S13(minimal) + analytic fixture [= the vertical
slice] → S10 complete → S11 latency spike → S11 → S12 → e2e + live gate.**

Why this order minimizes architectural risk:

1. **Consumer-validates-the-frozen-contract first.** Phase 0's rule was
   most-irreversible-first; nothing in this milestone is byte-irreversible, so the rule
   transposes: the pure grader is the cheapest probe of the *already-frozen* schema's
   adequacy (are `grid_track`'s fields sufficient? is `downbeat_t` workable?). If the
   contract has a flaw, it must surface from 200 lines of pure code — not after a
   threaded capture UX has been built against it.
2. **One lattice truth exists before its second consumer.** S9 is proven by the grader
   before the click consumes it, so the click and the grade cannot disagree by
   construction.
3. **The only new native dependency (PortAudio) is isolated and probed.** The click's
   pure schedule is tested before any audio; the latency spike produces a measured
   jitter number before UX work depends on the click being good enough.
4. **The concurrency-sensitive part is last.** S12's ordering protocol lands against a
   long-proven writer, a tested click, and a tested grader — every failure it can
   produce is observable through already-trusted tools (`replay`, `analyze`).

## 8. The smallest vertical slice

**Slice: S9 + S10-minimal (per-segment signed deviations, mean offset, std) +
S13-minimal (`edrum analyze` printing rush/drag per segment) + one analytic synthetic
fixture — run against the existing Phase 0 `declarations.jsonl` fixture *and* the new
fixture.** No audio, no keyboard, no new threads, no hardware.

Assumptions this ~small amount of code validates:

- the frozen declaration schema and fold output are *sufficient inputs for grading*
  (the single most load-bearing untested claim left from Phase 0);
- `downbeat_t`/`subdiv` semantics (micro-decision 8) actually support exact lattice
  reconstruction from a file alone;
- the calibration order-of-operations and sign convention (micro-decision 7) on both
  calibrated and uncalibrated meta;
- the claim-bounding result shape (n, `None`-not-zero, explicit empty-grid refusal) —
  the §0 governing principle expressed in code for the first time;
- engine purity and the thin-caller seam survive contact with real analysis;
- the golden-value regression harness (the §10 standing-corpus mechanism for numbers
  rather than bytes).

Why this is the correct first target: it has the highest ratio of
architectural-assumptions-validated to code-written available anywhere in the
milestone — every line is pure, deterministic, and testable in milliseconds, yet it
touches the schema, the fold, the profile layer, the purity boundary, and the claim
policy at once. And S9, which it proves, is the shared foundation the click then
inherits for free.

## 9. Future-phase audit

Zero schema changes; no new stored artifacts; no new invariants that later phases must
work around. Per phase:

| Future phase | Constraint on this milestone | How honored |
|---|---|---|
| P2 Layer D | needs attributable per-session metric atoms | `SessionAnalysis` carries provenance (`analysis_version`, profile id+version, calibration status, n) — D aggregates these objects; no cache, no stored grades (Tier 3 door stays closed) |
| P3 gestures | same declarations from a different input | `Command` protocol is input-agnostic; gesture recognizer = second `control_input`. Trigger-hit-span field = **additive** meta on declaration records (ignore-unknown-fields policy — verified additive); S10 reserves an exclusion-predicate parameter (documented, unbuilt) |
| P4 Layer B | recompute Layer A against inferred grids | `analyze_session(segments=…)` — B passes inferred segments and wraps results with confidence; B never forks the metric code. `n_ambiguous` is B's motivating signal |
| P5 Layer C | beat-relative folding; enrollment corpus | `downbeat_t` beat-anchoring (micro-decision 8) preserves beat phase; S9 exposes beat indices; `e` enroll command starts accumulating real enrollment spans *now*. Open gap: bar length (D6) — additive when resolved |
| P6 multi-drummer | per-note primary stats; profile as view | velocity/deviation keyed by raw note with lane views derived through the re-pointable profile — re-pointing changes lane views without touching stored data (tested) |
| The box | firmware must reproduce the loop | S9's math = the firmware click's reference; micro-decision 13's rules = firmware declaration behavior; S12's scripted acceptance files = firmware conformance cases (extending Phase 0's fixture-as-conformance-suite story) |
| P7 live drift | must be a thin consumer of the same stream | S10's grid-bound tier is a pure function over (events, segment) — an incremental caller can reuse it without a parallel path |

**Assumptions future phases now depend on** (stated so they're deliberate):
(1) declaration schema sufficiency — grid/enroll records need no new *required* fields;
(2) `downbeat_t` is beat-anchored in every file produced from now on;
(3) results-never-stored holds even for Layer D (aggregation is always recompute);
(4) the `Command` protocol is the stable seam gestures plug into;
(5) `ANALYSIS_VERSION` bumps whenever golden values change (D's discontinuity
explanations depend on it).

## 10. Open architectural decisions

| ID | Decision | Why it matters | Recommended default | Tradeoffs | Wait? |
|---|---|---|---|---|---|
| D1 | Deviation sign, lattice, membership, stats semantics (micro-decisions 7–10) | Freezes the *meaning* of every number the product will ever show; golden values weld it | As specified above | Committing now welds semantics into the regression corpus — but they are derived (recomputable), so a change costs an `ANALYSIS_VERSION` bump, not a corpus break | **No — decide now** (this plan's approval is the decision) |
| D2 | Calibration measurement method | Absolute rush/drag is meaningless without it; a wrong method (tap-along) would *measure the player's bias into the correction*, silently deleting the signal the product exists to measure | Ship: manual `--calibration N` (exists) + opt-in `--calibration auto` writing the PortAudio-reported output-latency estimate (+ small fixed MIDI-in allowance). Never write an estimate silently; `null` stays honest. Loopback measurement deferred | Manual = friction + user error; auto-estimate = plausible but unverified constant; loopback = honest but needs cabling/mic. Distinguishing measured-vs-estimated in meta would need an additive field — deferred until D needs it | Engine side proceeds now (subtract + flag). Routine can land late in the milestone or slip without blocking |
| D3 | Ambiguity threshold (0.35·tick) | The one modeling choice smuggled into the "honest floor" — it defines when nearest-line assignment is self-admittedly unreliable | 0.35·tick, named constant, surfaced in results | Any scalar is arbitrary; omitting it is worse (wrong-grid grading would look confident). Layer B replaces the heuristic with real inference | Proceed; revisit at Layer B |
| D4 | Click sound design (beat accent, no bar accent) | No time signature exists in the schema, so bar accents are unrepresentable — accenting bars would invent structure the data doesn't carry (§0) | Accent beats, quieter subdivision ticks, synthesized bursts | Drummers may want bar accents → that's D6's additive field, later | Proceed |
| D5 | BPM bucketing for trends | Cross-session comparability at nearby tempos | Defer entirely to Phase 2; exact `(bpm, subdiv)` grouping now (declared tempo is exact) | None at this phase | Wait (Phase 2) |
| D6 | Bar length / time signature field | Layer C folds "onto one bar" — bar phase is currently underivable except from the groove's own periodicity; bar-accented clicks also want it | Defer; when needed, an **additive** optional field on `grid_start`/`enroll_start` (reader policy makes it minor evolution) | Adding now = speculative schema surface; deferring = Layer C may need periodicity inference anyway (it needs it for recognition regardless) | Wait (Layer C design time) |
| D7 | numpy adoption point | Engine dependency-freedom vs. numeric convenience; affects the on-device-port story | Stdlib through Layer A/D; numpy/scipy at Layer B where clustering earns it | Stdlib float loops are slower — irrelevant at 10³–10⁴ events/session | Proceed stdlib |
| D8 | Audio replay (§4B) slotting | Must-have with no phase assigned — a real spec gap; replay is the adjudication credibility layer | Slot as its own milestone after Phase 2 (Layer D), before Layer B | Earlier = better trust loop, later = faster analytics ladder; it blocks nothing either way (schema asks nothing — §4B) | Wait; slot at next planning point |
| 2a / 9c | App runtime; gesture location | unchanged from spec | unchanged | — | Wait (2a); 9c partially resolved by this plan (keyboard = laptop control input; gestures Phase 3) |

## 11. Architectural self-review (adversarial pass)

- **Unnecessary abstraction, found & removed:** a `ClickEngine` protocol/ABC (only
  other implementer is firmware, in C — decoration, cut); a "metrics registry" or
  plugin system for Layer A (a classic temptation — metrics are plain functions +
  dataclasses; Layer D consumes dataclasses, not a framework — **explicitly
  forbidden**); histogram binning in engine results (presentation, pushed to callers);
  a `CalibrationSubsystem` (collapsed into a CLI flag + one engine rule + D2).
- **Hidden coupling, found & resolved:** click ↔ grader through duplicate lattice math
  (the worst one — resolved structurally by micro-decision 12 and the "no `60000/`
  outside `grid.py`" review rule); control ↔ click via internals (resolved: declarations
  read an immutable `ClickSnapshot` value, never click state); grader ↔ click (none
  exists and none may: the grader reads only the file — if the click misrenders, the
  file is still the truth of what was *declared*, and calibration/jitter measurement is
  where render fidelity is policed).
- **Duplicated responsibility, examined & kept:** capture writes explicit span ends
  *and* the fold auto-closes. Not duplication of truth — the fold rule is the semantic
  authority; the capture-side explicit end is the normal path and the fold is the crash
  safety net. Deliberate redundancy, documented in micro-decision 13.
- **Purity check:** `engine/analyze.py` takes objects, returns dataclasses; result→dict
  is pure; no paths, prints, or imports of io/cli anywhere in engine (existing
  structural rule + tests). The one subtle purity trap — reading click state during
  analysis — is impossible by construction (engine can't see io).
- **Concurrency honesty:** the staging protocol (micro-decision 14) is the piece a
  fast implementation would improvise wrongly. It exists because the alternative is a
  hard crash of a live recording on a µs race; it is ~15 lines and fully testable.
  Reviewed for overengineering and kept — but the *single-consumer* loop design (no
  locks, no second writer thread) is the actual simplification that makes it small.
- **Future migration risks:** golden *values* weld numeric semantics — intentional,
  with `ANALYSIS_VERSION` as the sanctioned change mechanism. The `[audio]` extra keeps
  PortAudio out of every non-recording workflow, so file analysis remains installable
  anywhere (the phone/server story). Keyboard input is Windows-only (`msvcrt`) — an
  io/cli leaf, swappable without touching the command protocol.
- **Simplification opportunities taken:** no schema change at all (the milestone's
  strongest longevity property — the corpus format is untouched); free-span
  enumeration cut from v1 (session-wide grid-free stats + per-segment grades cover the
  need; the complement computation waits for a real consumer); no config surface for
  the keymap; enrollment/bookmark commands included only because they are the *same
  mechanism* as grade spans (three commands, one code path).
- **Residual risks, stated:** (1) WASAPI click jitter could exceed ~2 ms — probed by
  the S11 spike *before* dependent work; if it fails, the fallback is exclusive-mode
  WASAPI or smaller buffers, not an architecture change. (2) The 0.35·tick ambiguity
  constant is a heuristic wearing an honest label — acceptable because it only ever
  *adds* doubt to a claim, never confidence. (3) Keyboard declarations add ~ms of
  human-scale latency to span boundaries — irrelevant to metrics (segment membership
  shifts by ms at boundaries where no metric is sensitive).

## 12. Acceptance criteria (milestone gate)

1. All Phase 0 tests remain green; Phase 0 fixtures byte-identical (zero-schema-change
   claim, tested).
2. Analytic golden-value suite passes (1e-6) on the new synthetic graded fixture.
3. Claim-bounding behaviors: empty grid → explicit refusal; n<2 → `None` std;
   uncalibrated → flagged; ambiguous fraction surfaced.
4. Headless e2e: scripted commands + FakeSource through the real graded loop → file →
   `replay` byte-identity PASS → `analyze` produces expected numbers; crash-cut
   variant recovers with fold auto-close.
5. Capture-side integrity invariant: click sounding, nothing declared → zero grid
   lines in the file.
6. Click latency spike: measured constant offset documented; jitter ≲ 2 ms.
7. Live gate (manual, kit + speakers): record warmup (click, undeclared) + a declared
   metronomic take + a declared intentionally-degraded take in one session →
   `analyze` ranks the takes correctly, warmup shows no grid; **drummer check:** the
   rush/drag sign and magnitude match felt sense.

## 13. Status

- [ ] Plan approved; micro-decisions 7–15 finalized; D1 resolved by approval, D2 default accepted
- [ ] Spec edits from §1 applied (living document)
- [ ] Vertical slice: S9 + S10-minimal + S13-minimal + analytic fixture green
- [ ] S10 complete (velocity, IOI, lane views) + golden-value suite
- [ ] S11 latency spike measured & documented
- [ ] S11 click engine + manual steadiness check
- [ ] S12 control surface + graded loop + headless e2e + race test
- [ ] Live gate (manual, hardware) passed
- [ ] `pyproject.toml`: `[audio]` extra added; no engine dependencies added
