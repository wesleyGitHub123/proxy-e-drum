# Phase 1 (revised) — The Statistics Layer: Architectural Review & Implementation Roadmap — PROPOSED

*Drafted 2026-07-12. Supersedes `phase1-plan.md` (2026-07-07), which is retained as a
historical design record — most of its analysis-side content survives here; its
capture-side content is obsolete because the firmware now provides that half of the
system. The product contract remains `edrum-brain-spec.md` + `edrum-capture-spec.md`.
Nothing here changes the frozen session-file schema except one **additive** field
proposed in F1 (trigger spans), which rides free under the ignore-unknown-fields
reader policy.*

---

# Part A — Architectural review: what the firmware made obsolete

## A.1 The world the original plan was written for, vs. the world today

The original `phase1-plan.md` was drafted the day after Phase 0 shipped, when the
capture half existed only as `edrum record` on the laptop. Its central (and then
correct) move was a scope correction: *Layer A cannot be validated without a grid
producer, and no grid producer exists — so pull declaration plumbing and a host click
forward from Phase 3.* Roughly half its engineering was that pulled-forward capture
surface: S11 (a `sounddevice` click engine + WASAPI latency spike), S12 (keyboard
control surface, graded-capture loop, and the micro-decision-14 staging/ordering
protocol), plus a calibration routine built around PortAudio latency estimates.

That premise is gone. The firmware is now capture implementation #2 as specced — and
it is the *better* one, validated and natively tested end to end:

| Capability the old plan had to build | What provides it today |
|---|---|
| Click as tempo authority on the one clock | `ClickScheduler`/`ClickRenderer` → PCM5102 → TD-02 MIX IN; same `Esp32Clock` instance stamps capture and schedules click (capture spec §4, milestone I.1) |
| A declaration producer (grade/enroll/bookmark) | Gesture grammar in firmware (kick+crash ×2/×3/×4) + console commands, both through `ControlMsg` → `ControlDispatcher` → session FSM, with the click-snapshot refusal path (capture spec §5/§12) |
| Session lifecycle | Auto-start on first hit, idle-timeout close, explicit ends, fail-soft storage (capture spec §6, §13.2) |
| The file itself | Byte-exact serializer, conformance-frozen against the brain's golden fixtures (`test_conformance`) |
| Getting files to the brain | `edrum sync` archive replication (COBS/CRC-16 framed serial, byte-identity verified on landing; capture spec §13) |
| Micro-decision 13's capture rules | Implemented in the firmware FSM/dispatcher (reject-without-click, param-change split, close-on-stop, explicit ends) |
| Micro-decision 14's staging protocol | Never needed — the box's ring/consumer design makes it moot; retired unbuilt |

**Verified against the code during this review** (not assumed): the serializer's meta
line carries nullable `calibration_offset_ms` from firmware config
(`serialize.cpp`, `config.h`); grid/enroll declarations carry exactly
`bpm/subdiv/downbeat_t` (`records.h`); the click snapshot anchors `downbeat_t` at the
**first beat edge at or after** the declaration (`click_sched.h
next_edge_at_or_after`, dispatched in `control_dispatch.cpp`); the gesture grammar
uses a 120 ms chord window and 700 ms sequence gap with emission on timeout
(`gesture.h`); and a real box-produced session (schema v2, ~1.5 MB) already exists
with `calibration_offset_ms: null` and a 1970 `start_iso` (pre-`settime` cold boot).

## A.2 Assumptions now obsolete

1. **"The laptop is the capture device."** Dead. The box is the primary producer;
   `edrum record` remains as producer #2 for ungraded laptop capture but is no longer
   on any critical path. Everything the old plan built *because* the laptop had to be
   the capture half — S11, S12, the `[audio]` extra, `msvcrt` keyboard polling, the
   WASAPI latency spike, micro-decision 14 — is deleted scope. If a no-box graded
   workflow is ever wanted (e.g. another drummer pre-box, Phase 6), it returns as its
   own optional milestone; nothing in this plan blocks or depends on it.

2. **"Layer A can only be validated synthetically until a grid producer exists."**
   Inverted. The validation gate is now *better* than the one the old plan designed:
   play on the real kit, gesture-declare spans against the real click, idle-close,
   `edrum sync`, `edrum analyze`. The live gate exercises the actual product loop.

3. **"The brain's grid math is the reference the firmware click will later match."**
   Reversed. The firmware click exists and is the *producer*; its written declarations
   define the semantics the brain must honor. Concretely: old micro-decision 8 said
   declarations snapshot "the most recent beat tick ≤ the declaration's `t`"; the
   firmware anchors at the first beat edge **at or after** it. The load-bearing
   property — `downbeat_t` is **beat-anchored** (a beat edge, never a subdivision
   edge) — is honored by the firmware (`beat_us` uses only BPM). The direction is
   immaterial because the lattice is unbounded in both directions (`downbeat_t +
   k·tick_ms` for all integers k); beat indices shift by an integer, deviations are
   identical. Amendment 8′ in Part C fixes the wording; `engine/grid.py` must not
   assume `downbeat_t ≤ start_t` (it can legitimately be after the declaration).

4. **"Calibration is a per-laptop-setup moving target (PortAudio estimates)."** On
   the box the offset is a hardware constant: I2S DMA depth (~5.3 ms) + MIX-IN chain
   + ~1 ms USB MIDI-in floor. It can be measured **once per hardware revision**
   (Experiment 7's DOUT-vs-debug-GPIO measurement supplies the audio half) and set in
   `/config.txt`, after which every session is stamped calibrated. The engine-side
   rule is unchanged (subtract per-session meta value; `null` → 0 + flagged
   uncalibrated). See F2.

5. **"Trigger-hit exclusion can wait for Phase 3."** No longer true — this is the one
   place the firmware work *forces* something forward rather than deleting it. The old
   plan deferred exclusion because keyboard declarations involve no pad hits. But
   every real declaration is now gesture-produced: 2–4 kick+crash chords (4–8 hard,
   off-grid events). Start-side chords land *before* `start_t` (the declaration
   stamps at the 700 ms emission timeout), so they miss the grid-bound tier — but
   **end-toggle chords are played inside the open span** and land in every
   gesture-closed graded segment, and *all* gesture chords contaminate grid-free
   velocity/IOI stats and, later, Layer C enrollment folds. Spec §5 already says the
   engine excludes trigger hits by default and the declaration carries the span; the
   firmware doesn't write one yet. F1 (additive field) + the exclusion substrate in
   S14 close this now, before the corpus grows.

6. **"The Layer-D corpus clock starts when this milestone ships."** It has already
   started — the box records and syncs real sessions today. This strengthens the
   A → D ordering and makes Phase 2 nearer than the old plan assumed. It also
   surfaces a data-quality wart Phase 1 should acknowledge: pre-`settime` cold-boot
   sessions carry epoch dates (one already exists). The analysis report flags
   obviously-bogus `start_iso` as `undated`; Layer D treats ordering for such files
   as a first-class quality problem (ADR-5's sync-time injection heals it going
   forward).

## A.3 What survives unchanged (the principles inventory)

Everything below is preserved verbatim from the spec and the old plan, and this
roadmap is designed *around* it:

- **Engine purity** (§2): records in, results out; no I/O, paths, or printing in
  `engine/`; the CLI stays a thin caller.
- **Grading is derived, never stored** (§1A): no result is ever written into a
  session file; recompute is the only path. The Tier-3 cache door stays closed
  (see B.8).
- **Claim bounding** (§0, §7): `None`-not-zero, n everywhere, explicit refusals,
  the hand-identity wall enforced at the type level.
- **One lattice truth** (old micro-decision 12): all lattice arithmetic brain-side
  lives in `engine/grid.py`; no `60000 /` anywhere else. The firmware scheduler is
  the producer-side twin; golden fixtures keep the two honest.
- **Metric semantics** (old micro-decisions 7, 9, 10, 11, 15): deviation sign and
  calibration order; segment membership; sample std ddof=1 with `None` under n<2;
  the 0.35·tick ambiguity signal; results as unfrozen shapes with golden *values*;
  raw-note-primary stats with lane views derived through the re-pointable profile.
- **Stdlib-only engine through Layers A/D** (D7): numpy earns its keep at Layer B.
- **Golden-value regression discipline** (§10): the validation gate a milestone
  passes becomes its permanent test.

---

# Part B — The statistics architecture

## B.1 Design philosophy, and an honest revision of one old prohibition

The old plan's adversarial self-review "explicitly forbade" a metrics registry or
plugin system: *metrics are plain functions + dataclasses*. That prohibition was
correct **for its scope** — one layer of metrics, known in advance, consumed by one
CLI. The requirement has changed: the explicit product constraint is now *months from
now, think of a new metric, write one module, plug it in, run it over the existing
corpus, refactor nothing*. That is a real requirement, not speculative generality —
so it earns a mechanism.

The resolution is a **thin declarative layer over plain pure functions**, not a
framework. What changes: analyzers gain an identity, a version, and a declared
dependency set, and a ~100-line runner resolves ordering and stamps provenance. What
stays forbidden: dynamic plugin discovery (entry points, directory scanning), a
configuration DSL, analyzer base-class hierarchies, result persistence, and any
framework the engine itself must be taught about. Registration is an explicit import
plus one line in an explicit list — "almost effortless" without being magic.

Three ideas carry the whole design:

1. **Primitives are geometry; derived metrics are compressions of it** (§1A made
   architectural). The primitive tier produces *tables* — per-event rows drawable
   directly on the piano roll (event ↔ gridline ↔ signed deviation). Summary numbers
   (rush/drag, consistency) are downstream analyzers that compress those tables.
   Visualization and longitudinal analysis therefore reuse the same objects the
   metrics are computed from — nothing is computed twice, and every number remains
   adjudicable by looking at the geometry that produced it.

2. **Dependency declaration does triple duty.** An analyzer's `requires` block is
   (a) the DAG edge set for ordering, (b) the **claim-bounding gate** — an
   unsatisfiable requirement (no grid segments, no profile) yields a structured
   `Refusal`, never a fake number and never a crash — and (c) the provenance record:
   a result knows exactly which inputs, at which versions, produced it.

3. **Quality is plumbing, not discipline.** Sample sizes, calibration status,
   exclusion counts, and (later) inference confidence propagate through the runner
   mechanically — a derived metric can never *forget* to inherit its inputs'
   uncertainty, because inheritance is done to it.

## B.2 Data flow

```
session.jsonl ──read_log──▶ records ──reduce_session──▶ Session (parsed reduction)
                (recovery)                (fold)            │
profiles/td02k.json ──load_profile──▶ KitProfile ──────────┤
                                                            ▼
                                              AnalysisInputs (substrate, built once)
                                              • session (immutable)
                                              • profile + lane views
                                              • effective events (trigger-span marks)
                                                            │
                              ┌─────────────────────────────┤
                              ▼                             ▼
                     Tier-1 primitive analyzers    requirement checks
                     (deviation table, velocity    (unsatisfied → Refusal)
                      observations, IOI table)
                              │  results by id
                              ▼
                     Tier-2 derived analyzers
                     (rush/drag, consistency,
                      velocity stats, IOI summaries)
                              │
                              ▼
                     AnalysisReport (results + refusals + provenance header)
                              │
              ┌───────────────┼───────────────────┐
              ▼               ▼                   ▼
        `edrum analyze`   future viz        Layer D corpus scan
        (table / --json)  (draws Tier-1     (groups results by
                           tables on the     analyzer_id × scope
                           timeline)         across sessions)
```

Everything from `AnalysisInputs` rightward is pure (`engine/`); everything left of it
is existing Phase 0 io. The pipeline is per-session by design; cross-session work
(Layer D) is a thin runner that maps this pipeline over files and consumes the
reports — same envelope, no new result model.

## B.3 The substrate: `AnalysisInputs`

Built once per run by the runner (pure function of `Session` + `KitProfile`):

- **`session`** — the parsed reduction, untouched.
- **`profile` / lane views** — normalization applied on read (§4.2), never stored.
- **`effective_events`** — the performance track with **exclusion marks**, not
  deletions: each event is annotated excluded/included based on the union of declared
  trigger spans (F1 field, when present). Raw is preserved; exclusion is a view.
  Primitives carry the mark through to their rows; summaries skip marked rows and
  report `n_excluded`. Enrollment-span trigger hits get the same treatment, which
  Layer C inherits for free.

This is the entire parsing↔analysis seam: analyzers see `AnalysisInputs` and upstream
results, nothing else — no paths, no files, no registry access.

## B.4 The analyzer contract

An analyzer is a frozen descriptor around a pure function:

- **`id`** — stable dotted string (`"timing.deviations"`, `"timing.rush_drag"`,
  `"velocity.per_note"`, `"ioi.pairs"`, …). Never reused, never renamed (Layer D
  time series key on it).
- **`version`** — integer; bumps when the analyzer's *numeric behavior* changes
  (the per-analyzer analog of `ANALYSIS_VERSION`, which stays as the engine-wide
  umbrella; a Layer-D discontinuity must always be attributable to a version bump).
- **`requires`** — declarative: `needs_grid`, `needs_profile` (flags checked against
  the inputs; unsatisfied → `Refusal(analyzer_id, reason)`), and `upstream:
  (analyzer ids…)` (DAG edges; a missing/refused upstream propagates refusal with
  the chain recorded).
- **`compute(inputs, upstream_results) -> list[MetricResult]`** — pure, deterministic,
  stdlib-only at this phase; iterates in `t`-order for float determinism.

Registration: `engine/analyzers/__init__.py` holds an explicit `ALL = [...]` list;
adding a metric = one new module + one import line. The runner
(`run_analyses(session, profile=None, analyzers=None)`) topologically orders the
requested subset, evaluates, and assembles the report. Cycles are a programming
error (assert), not a runtime condition.

## B.5 The result envelope

One uniform shape so that the CLI, future visualizations, and Layer D all consume
results without knowing any analyzer's internals:

- **`Scope`** — *flat typed axes, not nested objects*: optional
  `span` (a segment/enrollment reference: kind + `start_t`/`end_t`), optional
  `lane` (instrument), `articulation`, `note`, `bpm`, `subdiv`. A session-wide
  metric leaves all axes `None`. Flatness is the load-bearing choice: cross-session
  alignment ("rush/drag per lane per (bpm, subdiv) over weeks") becomes a mechanical
  group-by on `(analyzer_id, axes…)` with no per-analyzer knowledge.
- **`Quality`** — `n`, `n_excluded`, `n_ambiguous`, `calibrated: bool`,
  `confidence: float | None` (**`None` means "deterministic — exact given the
  data"**, deliberately distinct from any numeric confidence; only inference
  analyzers ever populate it), `flags: tuple[str, ...]`.
- **`MetricResult`** — `analyzer_id`, `analyzer_version`, `scope`, `unit`
  (`"ms"`, `"velocity"`, …; cheap, and viz wants it), `value` (a float, an int,
  `None`, a small frozen dataclass, or a tuple of rows for Tier-1 tables), `quality`.
- **`Refusal`** — `analyzer_id`, structured `reason` (`no_grid_segments`,
  `no_profile`, `upstream_refused(chain)`, …). A refusal is a *result* — the §0
  principle reified — and the CLI prints it as information, exit 0.
- **`AnalysisReport`** — provenance header (`session_id`, `schema_version`,
  `ANALYSIS_VERSION`, `(kit_profile_id, profile_version)` when used, calibration
  status, fold warnings, dating flag) + `results` + `refusals`.
  `report_to_dict()` is a pure engine function; shapes remain deliberately unfrozen
  (micro-decision 11) — golden **values**, not golden bytes, guard them.

## B.6 Metric tiers

- **Tier 0 — substrate** (B.3): not metrics; built once.
- **Tier 1 — primitive tables** (deterministic, per-session):
  `timing.deviations` (per graded event: segment ref, event `t`, corrected `t`,
  nearest gridline, lattice index k, signed deviation, ambiguous mark, excluded
  mark), `velocity.observations` (per event: note, lane view, velocity, excluded
  mark), `ioi.pairs` (per consecutive same-scope pair). These are the §1A drawable
  geometry and the *only* place the raw arithmetic happens.
- **Tier 2 — derived summaries** (deterministic, per-session): rush/drag (mean
  signed deviation) and consistency (std) per segment / per lane / per note / per
  `(bpm, subdiv)`; velocity mean/spread per note with lane aggregation via profile;
  IOI distribution summaries. Consume Tier 1 by id; contain no lattice math.
- **Tier 3 — cross-session** (Phase 2 / Layer D): trend, effect size, uncertainty
  bands — analyzers over *collections of reports*, keyed by `(analyzer_id, scope
  axes)`. The envelope is designed so this tier needs no new result model.
- **Tier 4 — inference** (Layers B/C): confidence-carrying analyzers (see B.9).

Histogram binning stays presentation-side (callers bin Tier-1 tables); the engine
returns raw lists alongside summaries, as the old plan already resolved.

## B.7 Confidence & quality propagation

One mechanical rule, enforced by the runner (the meet of inputs, then the analyzer's
own contribution):

- `calibrated` = AND over upstream inputs (and the session's own status for Tier 1).
- `confidence` = min over upstream non-`None` confidences; stays `None` iff every
  input is `None` and the analyzer is deterministic. A deterministic Tier-2 metric
  recomputed over an *inferred* grid (Layer B) therefore automatically carries the
  grid's confidence — B wraps A without A changing.
- `flags` = union (e.g. `uncalibrated`, `high_ambiguity`, `contains_excluded`,
  `undated`).
- `n` / `n_excluded` / `n_ambiguous` are the analyzer's own (they describe its
  sample), but omission is impossible by construction — the envelope requires them.

## B.8 Immutable vs. cached

- **Immutable, authoritative:** session files (append-only, never rewritten), golden
  fixtures, versioned kit profiles. These are the only truth.
- **Never stored, always recomputed:** every `MetricResult` and `AnalysisReport`.
  Phase 1 builds **no cache** — sessions are 10³–10⁴ events and stdlib arithmetic is
  effectively instant.
- **The future cache (Tier 3 door, kept closed but keyed):** if Layer D
  recompute-over-corpus ever gets slow, the sanctioned mechanism is a rebuildable,
  non-authoritative index keyed by `(file bytes hash, analyzer_id, analyzer_version,
  ANALYSIS_VERSION, kit_profile_id, profile_version, schema_version)`. Every
  component of that key is *already stamped on every result* by this design — the
  provenance envelope is the cache-key discipline paid forward at zero cost, so
  adding the cache later is mechanical and can never silently serve stale numbers.

## B.9 Where ML and probabilistic analyses slot in

Inference analyzers (Layer B subdivision/grid inference; Layer C matching) are just
Tier-4 registry entries with three extra properties, none of which touch the
deterministic tiers:

1. **They populate `confidence`** — the envelope already carries it; downstream
   propagation (B.7) is already mechanical.
2. **They may produce *alternative inputs*, never replacements.** Layer B's output is
   an inferred grid track (segments tagged with confidence and
   `grid_source: inferred`). The runner re-evaluates grid-bound Tier-1/2 analyzers
   against those segments under a **distinct scope axis** (`grid_source`), producing
   *additional* rows. Declared-grid results are computed from declared segments only
   and are byte-for-byte unaffected by any inference — the isolation guarantee is
   structural, not disciplinary.
3. **They may carry heavy dependencies** (numpy/scipy at Layer B, per D7), isolated
   in their own modules; the registry list for the deterministic tiers never imports
   them. Layer C additionally consumes `enrollment_spans` (already accruing from the
   box) and the brain-owned groove-identity mapping (spec §6 Phase 5) — its profile
   store remains a rebuildable derived index, exactly as specced.

The engine's *refusal* machinery is also where inference is claim-bounded: a Layer-B
analyzer whose IOI evidence is too sparse refuses (structured reason) rather than
emitting a low-confidence guess by default.

## B.10 Deliberately not built

No dynamic plugin discovery; no analyzer config files; no result persistence of any
kind; no cross-session machinery yet (Phase 2); no plotting in engine (a CLI `--plot`
stretch may consume Tier-1 tables); no realtime/incremental evaluation (Phase 7's
live drift consumer can call the same pure functions later); no laptop click or
keyboard control surface (obsolete, A.2); no MIDI export; no numpy.

---

# Part C — Semantic micro-decisions: dispositions and amendments

Numbering continues the phase0 (1–6) and old-phase1 (7–15) registers. Frozen
*meanings*, guarded by golden values.

| # | Disposition |
|---|---|
| 7 (deviation sign, calibration order) | **Kept verbatim.** `t_corrected = t − calibration_offset_ms`; positive = late/drag; correction before nearest-line assignment; `null` → 0 + `calibrated=False`. |
| 8 → **8′ (amended)** | **Lattice kept; anchoring wording corrected to match the producer.** `downbeat_t` is a **beat edge** of the running click as written by the producer — the firmware anchors at the first beat edge *at or after* the declaration (`next_edge_at_or_after`). The lattice `downbeat_t + k·tick_ms` is unbounded in both directions, so anchor direction is immaterial to grading; beat-anchoring (never subdivision-anchoring) remains the load-bearing property, and `grid.py` must not assume `downbeat_t ≤ start_t`. `subdiv` = ticks per beat (1/2/3/4…), unchanged. |
| 9 (segment membership, boundary rule) | **Kept.** Inclusive `[start_t, end_t]`; later segment wins a shared boundary. |
| 10 (stats honesty) | **Kept.** ddof=1; `None` under n<2; no outlier rejection; ambiguity `|dev| > 0.35·tick` surfaced as `n_ambiguous`. |
| 11 (results unfrozen shapes, golden values, provenance) | **Kept and extended** by the B.5 envelope; `ANALYSIS_VERSION` retained, joined by per-analyzer `version`. |
| 12 (one lattice truth) | **Kept, restated:** all brain-side lattice math in `engine/grid.py`; no `60000 /` outside it. The click-renderer half of the old wording is obsolete (the firmware is the renderer); conformance between the two lattice implementations is held by golden values over firmware-produced fixtures. |
| 13 (capture declaration rules) | **Retired as brain scope** — implemented in firmware (dispatcher/FSM); the fold's auto-close remains the crash safety net. |
| 14 (staging/ordering protocol) | **Retired unbuilt** — laptop graded-capture loop no longer exists. |
| 15 (metric groupings) | **Kept** as the Tier-2 scope set: overall per segment + per raw note + per lane (profile-dependent view); raw lists returned alongside summaries. |
| **16 (new) — result envelope** | `Scope` is flat typed axes (B.5); `analyzer_id` is a permanent key (never renamed/reused); `Quality.confidence = None` means deterministic, and only inference analyzers may set it. |
| **17 (new) — quality propagation** | The runner computes the meet (B.7): AND on `calibrated`, min on `confidence`, union on `flags`. Analyzers cannot opt out. |
| **18 (new) — trigger-hit exclusion** | Exclusion is a *mark on a view* (`effective_events`), never a deletion; source of truth is the declaration's `trigger_span` field (F1). Summaries skip marked events and report `n_excluded`; Tier-1 tables carry the mark. Sessions predating F1 are analyzed as-is (no heuristic un-marking by default — see D9). |
| **19 (new) — registry discipline** | Explicit list, explicit imports, no dynamic discovery. An analyzer is added by writing one module and one list entry; nothing else in the system changes. |

---

# Part D — Implementation roadmap

## D.1 Milestone P1-R2: subsystems

### S9′ — Grid geometry (`engine/grid.py`)
Unchanged from the old plan's S9 except amendment 8′: `tick_ms`/`beat_ms`;
`nearest_gridline` (returns gridline time + lattice index); `gridlines_in`/`beats_in`
over half-open ranges; `segment_for` with the boundary rule; pure, stateless,
property-tested (deviation ∈ (−tick/2, tick/2]; lattice invariance under
`downbeat_t ± k·tick`; anchor-after-start_t cases). The one place lattice arithmetic
exists brain-side.

### S14 — Analysis core (`engine/analysis.py`)
The envelope and the runner: `Scope`, `Quality`, `MetricResult`, `Refusal`,
`AnalysisReport`, `report_to_dict`; the `Analyzer` descriptor and `Requirements`;
`build_inputs(session, profile)` (substrate incl. `effective_events` from trigger
spans); `run_analyses(...)` (topo-order, requirement gates, quality meet, provenance
stamping); `ANALYSIS_VERSION = 1`. Stdlib only. Acceptance: scripted fake analyzers
prove ordering, refusal chains, and propagation independent of any real metric.

### S15 — The Layer-A analyzers (`engine/analyzers/`)
`timing.py` (`timing.deviations` Tier-1 table; `timing.rush_drag`,
`timing.consistency` Tier-2 per segment/lane/note/(bpm,subdiv)); `velocity.py`
(`velocity.observations`; `velocity.per_note`, `velocity.per_lane`); `ioi.py`
(`ioi.pairs`; `ioi.summary`). All semantics per Part C. Acceptance: the analytic
golden-value suite (1e-6) plus the claim-bounding behaviors (grid-empty refusal,
n<2 → `None`, uncalibrated flag, ambiguity and exclusion counts).

### S13′ — Analysis CLI (`cli/main.py`: `edrum analyze`)
`edrum analyze FILE [--json] [--kit ID] [--profiles-dir DIR] [--metrics id,...]` —
load, resolve profile from meta with `--kit` as the re-pointable override, run,
print per-segment tables + session stats + refusals + warnings, or emit
`report_to_dict` JSON. Zero logic beyond arg handling. Exit 0 on refusals (absence
of grid is truth, not error).

### F1 — Firmware: trigger spans on gesture-produced declarations *(small, additive)*
Gesture-emitted `bookmark`/`grid_start`/`grid_end`/`enroll_start`/`enroll_end` lines
gain `"trigger_span":[t_first,t_last]` (first chord's first hit → last chord's last
hit). Console-produced declarations omit the field, so **every existing golden
fixture stays byte-identical**; one new gesture-declared fixture is added to the
conformance suite. Additive under ignore-unknown-fields ⇒ no schema major bump.
Brain side: optional field on the declaration records, folded onto
segments/spans/bookmarks, consumed by S14's substrate.

### F2 — Firmware/config: stamp the box's calibration constant
After Experiment 7's offset measurement: set `calibration_offset_ms` in
`/config.txt` (config plumbing already exists — `has_calibration`); document the
measured number in the capture spec. All subsequent sessions land calibrated; the
engine's honest-`null` path continues to cover the existing corpus.

### Fixtures & golden values
`tests/fixtures/graded_synthetic.jsonl` built through the real pipeline with
`FakeClock`/`FakeSource`: hits at exact gridlines + known offsets {0, ±10, …},
a free-play span, calibrated + uncalibrated variants, a gesture-style trigger-span
declaration (post-F1), and hand-computed expected metrics asserted to 1e-6. Phase 0
fixtures untouched and byte-identical.

## D.2 Build order & the vertical slice

```
S9′ grid ──▶ S14 core ──▶ S15 timing (deviations + rush/drag) ──▶ S13′ minimal CLI
                                   │                                    │
                                   ▼                                    ▼
                        [ VERTICAL SLICE: run on declarations.jsonl,   analytic
                          anonymous_enroll.jsonl, the real box file,   fixture +
                          warmup_no_grid.jsonl ]                       golden values
                                   │
                                   ▼
              S15 complete (velocity, IOI, groupings) ──▶ full golden suite
                                   │
                     F1 (firmware + brain reader + exclusion live)
                     F2 (config constant, post-Experiment-7)
                                   │
                                   ▼
                          Live gate on the box (D.6.6)
```

**The slice** = S9′ + envelope + *one primitive and one derived analyzer* + minimal
CLI, run against the existing fixtures **and the real box-produced session**. Why
this exact cut: it is the first consumer of the frozen contract (probing `grid_track`
sufficiency and 8′ downbeat semantics against *firmware-written* values, not
synthetic ones), and it exercises the dependency seam — primitive→derived is the
architecture's whole bet, so it must be proven with two analyzers before ten exist.

## D.3 Phase re-mapping (what moved, and why)

| Item | Old home | New home | Why |
|---|---|---|---|
| Host click engine (S11), keyboard control surface (S12), staging protocol (md-14), `[audio]` extra, WASAPI spike | Phase 1 | **Deleted** (optional future "laptop graded capture" milestone only if ever needed) | The box is the capture half; building a second one has no consumer |
| Layer A engine + analyze CLI | Phase 1 | **Phase 1 (this plan)** | Unchanged core |
| Trigger-hit exclusion + trigger-span field | Phase 3 | **Phase 1 (S14 + F1)** | Gestures are live; every real graded span's end is contaminated without it; additive now = clean corpus forever |
| Gesture grammar | Phase 3 | **Done (firmware)** | Shipped, natively tested, specced (§5, 9c) |
| Phase 3 as a phase | — | **Dissolved** | Both halves are done or absorbed; remove from the spec's phase list |
| Calibration routine | Phase 1 (D2, PortAudio estimate) | **F2** (one-time measured constant) + existing engine rule | Box hardware made the moving target a constant |
| BPM bucketing decision | Phase 1 open | Phase 2 (unchanged from old plan's dissolution) | Declared tempo is exact; bucketing is a trend-time concern |
| Layer D (Phase 2) | After Phase 1 | **Next milestone, sooner** | Corpus accrues from the box already; the Tier-3 runner over `AnalysisReport`s is mechanical given B.5/B.6 |
| Audio replay (§4B) | Unslotted (old D8: after Phase 2) | **Confirmed: its own milestone after Phase 2** | Unchanged reasoning; consumes raw events + profile only |
| Layer B / Layer C / Phase 6 | Phases 4/5/6 | Unchanged order; C's corpus (enrollment spans) already accruing | Tier-4 slot-in per B.9 |

## D.4 Future-phase audit (constraints honored by this design)

| Future phase | Constraint | How honored |
|---|---|---|
| P2 Layer D | attributable, alignable per-session atoms | Envelope: flat scope axes + full provenance + per-analyzer versions; dating flag for epoch-dated files; no cache, but the cache key is pre-paid (B.8) |
| Audio replay | raw events + profile lane routing only | Untouched substrate; replay never reads results |
| P4 Layer B | recompute A against inferred grids, tagged | `grid_source` scope axis + alternative-input mechanism (B.9); `n_ambiguous` is B's motivating signal; refusal machinery bounds sparse-evidence claims |
| P5 Layer C | beat-relative folding; clean enrollment reps | 8′ beat-anchoring; `beats_in`/indices in S9′; enrollment spans get span-scoped stats and trigger-hit exclusion for free; groove identity stays a brain-owned overlay |
| P6 multi-drummer | raw-note-primary, profile as view | md-15 groupings; re-pointing the profile changes lane-scoped results only (tested — the SSD property at the metrics level) |
| The box | fixtures as conformance | F1's new fixture extends the suite; all existing fixtures byte-frozen |
| P7 live drift | thin consumer of the same stream | Tier-1/2 analyzers are pure functions over (inputs, segments) — an incremental caller reuses them without a parallel path |

## D.5 Open decisions

| ID | Decision | Recommended default | Wait? |
|---|---|---|---|
| D2′ | Source of the F2 constant | Experiment 7's DOUT-vs-GPIO measurement + fixed ~1 ms USB-in allowance; document both components in the capture spec | Lands with Experiment 7; engine proceeds regardless (null stays honest) |
| D3 | Ambiguity threshold 0.35·tick | Keep; named constant; revisit at Layer B | Proceed |
| D9 | Heuristic trigger-span backfill for pre-F1 sessions | **Don't.** The pre-F1 corpus is ~3 files of test data; the honest corpus effectively starts at this milestone. If Layer D ever wants those files, an explicitly-flagged offline matcher (kick+crash chord runs ending ≤ `seq_gap_ms` before a declaration `t`) can be added as an opt-in tool, never a silent default | Wait |
| D10 | Final scope-axis set | `span(kind,start,end)`, `lane`, `articulation`, `note`, `bpm`, `subdiv` (+ reserved `grid_source`) | Decide at S14 (this plan's approval) |
| D11 | Where the corpus runner lives | Phase 2; `io/` or `cli/`-side iteration over `sessions/` feeding pure Tier-3 analyzers | Wait (Phase 2) |
| D6 | Bar length / time signature | Unchanged: additive field when Layer C needs it | Wait |
| 2a | App runtime | Unchanged | Wait |

## D.6 Acceptance criteria (milestone gate)

1. All Phase 0 + devlink suites stay green; every existing golden fixture
   byte-identical (the brain side of this milestone is read-only w.r.t. the schema).
2. Analytic golden-value suite passes (1e-6) over the synthetic graded fixture.
3. Claim bounding: grid-empty file → structured refusal (exit 0); n<2 → `None` std;
   uncalibrated → flagged on every timing result; ambiguity and exclusion counts
   surfaced.
4. Real-file smoke: the existing box session analyzes clean — grid-free stats
   present, grid-bound tier refuses, `undated` flag raised, no crash, no fake
   numbers.
5. **Extensibility rehearsal (the point of the whole design):** add a toy analyzer
   (e.g. `density.hits_per_minute`) end to end — one new module + one registry
   line, zero edits elsewhere — and it appears in `edrum analyze` and `--json`
   output with full provenance. If this takes more than ~an hour or touches any
   existing file beyond the registry list, the architecture failed its own bar.
6. Live gate on the box: one session — click-accompanied warmup (undeclared) →
   gesture-declared metronomic take → gesture-declared intentionally-degraded take
   → idle close → `edrum sync` → `edrum analyze` ranks the takes correctly, warmup
   shows no grid, and (post-F1) end-gesture chords appear as `n_excluded`, not as
   crash-lane outliers. **Drummer check:** rush/drag sign and magnitude match felt
   sense.
7. F1 conformance: firmware serializer reproduces the new gesture-declared fixture
   byte-for-byte; native suites green.

## D.7 Spec edits to apply on approval (living documents)

1. Brain spec §9 Phase 1: rewrite "asks of capture" (arrival-edge timestamps, click
   authority, gesture/console declarations, sync — **all provided by the box**;
   drop the stale "BPM in meta"); validation = the D.6.6 box loop.
2. Brain spec §9 Phase 3: mark dissolved (gesture grammar shipped firmware-side,
   9c; trigger exclusion absorbed into Phase 1); renumber or annotate the phase
   list accordingly.
3. Brain spec §9: slot audio replay as its own milestone after Phase 2 (resolves
   the old D8 spec gap).
4. Brain spec §6 Layer A: note the analyzer/envelope architecture and amendment 8′.
5. Capture spec §5: document `trigger_span` (F1) as an additive declaration field;
   conformance fixture note.
6. Capture spec §4: F2's measured constant, once Experiment 7 lands.
7. Decision registers both sides: rows for 16–19, 8′, D9–D11; mark md-13/14
   retired.

## D.8 Status

- [x] Plan approved 2026-07-13; Part C dispositions finalized (approval froze
      7–12/15 as amended and 16–19 as new)
- [x] Spec edits (D.7) applied (brain spec §6/§9/§11; capture spec §5/§10;
      phase0-plan S1 additive amendment)
- [x] Vertical slice green (S9′ + S14 + timing primitive/derived + CLI, incl. the
      real box file: 6,421 events, honest refusals, `undated` flag, ~0.3 s)
- [x] S15 complete (timing/velocity/IOI + density) + full golden-value suite
      (brain: 190 tests green; goldens 1e-6; fixtures replay byte-identical)
- [x] F1 firmware + fixtures + brain reader; exclusion live end to end
      (firmware: 120 native tests green incl. new gesture-span/dispatch/serialize
      cases; graded_synthetic pair added to byte-conformance; ESP32 build green;
      also fixed: `test_fixture_anonymous_enroll` was defined but never RUN_TEST'd)
- [ ] F2 constant measured (Experiment 7, hardware) and configured
- [x] Extensibility rehearsal (D.6.5) passed — `density.hits_per_minute` landed as
      one module + one registry line; surfaced in CLI/JSON with zero other changes
- [ ] Live gate on the box (D.6.6) — hardware; pending alongside Experiments 7/8
