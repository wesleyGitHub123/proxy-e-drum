# E-Drum Practice Tool — Brain Architecture & Development Plan

*Living spec. Sections marked **DECISION** are open choices for you to resolve. Everything else is the agreed shape.*

---

## 0. The governing principle

One rule sits above the whole system. Every component obeys it, and it is the test for every later tech decision:

> **A claim is bounded by the information physically present in the stream. The system must always know which mode it is in, and make only the claims that mode's information justifies.**

Breaking this rule *is* the gimmick failure mode (confident output the data can't support). Preserving it is the product's integrity. When evaluating any library, model, or shortcut later, the question is: does this preserve the rule or quietly cross it?

---

## 1. The two-machine split

The system is deliberately two halves that never merge.

**Capture half** — C firmware on the MCU. Does exactly three things: host the kit over USB, timestamp each hit at the moment of arrival, persist `(timestamp, note, velocity)`. Realtime, deterministic, never blocks, *zero analysis*. The timestamp lives here by necessity, because it must share the MCU's clock with the click the MCU generates.

**Brain half** — Python on a real CPU (laptop now, phone/server later). Everything else: grading, inference, recognition, trends. Heavy, floating-point, latency-tolerant. Does not care if it runs now or an hour later — which is exactly why it must not run on the microcontroller.

**The only coupling is the session file.** The brain is *source-agnostic*: it consumes session records regardless of who produced them, so "kit → Reaper → laptop" today and "kit → capture box" later are interchangeable producers. This is what lets the entire brain be built now with no hardware.

```
   [ kit ] --USB--> [ CAPTURE half ] --session file--> [ BRAIN half ] --> metrics / matches / trends
                     timestamps,                        engine (pure)
                     click,                             + thin interface (CLI now, app later)
                     control input
```

---

## 1A. The bottom-layer contract — the timeline is the truth

This is the canonical form of the data. It sits upstream of the data model (§3) and the modes (§5) and refines both. Hold every later decision against it.

**The native shape.** A drum performance is notes on a time axis — the piano-roll / falling-notes / Guitar-Hero form. This is not a visualization chosen at the end; it is how drummers think, how music notation works, and how the data actually *is*. The event model `(t, note, velocity)` already *is* a piano roll: `note` = lane (pad → row), `t` = horizontal position, `velocity` = visual weight. So the timeline view is free — it's just plotting the raw events. The constraint this imposes: **anything the engine computes must be expressible as something drawable on that timeline, or it doesn't belong in this layer.**

**Two parallel tracks, one timeline.**
- *Performance track* — `(t, note, velocity)`, always recorded raw, continuous, no modes. The Jamcorder-equivalent capture; it never changes based on intent.
- *Grid track* — the gridlines of a **declared graded span** (BPM + subdivision + downbeat), snapshotted from the running click. Present **only** across spans the drummer explicitly declared as graded. Genuinely empty everywhere else — including warmup where the click may be sounding but no grade span was declared.

**Grading is derived geometry, not stored state.** A grade is the visible spatial relationship between a note and its gridline — rush/drag is the bar sitting left/right of the line, consistency is the scatter of bars around their lines, velocity unevenness is uneven bar heights. Metrics are downstream *compressions of that geometry*, recomputed on demand from the two tracks, never written into the file as facts. A drummer can always look at the roll and see *why* a number is what it is — which is what makes it adjudicable.

**Grading is a viewing lens, not a capture mode.** What got played is identical whether or not you grade it. So there is no ambient-vs-graded *capture* mode — there is one capture plus an optional grid track. The "grade this" declaration supplies exactly one thing: **the reference** (BPM + subdivision + downbeat) for a span, snapshotted from the running click. Crucially, *sounding the metronome and declaring a graded span are two different acts* — you can warm up to a click without grading it. Sounding the click writes nothing; **declaring a graded span is what writes the grid segment.** Everything else (offsets, rush/drag, consistency) is automatic once the grid exists.

**The no-friction view is the base case:** the object with the grid track empty — notes on an axis, scrub, listen, bookmark. A single session can be grid-empty for some spans (warmup — even to a click — free, rubato) and grid-present for others (declared graded spans), one continuous timeline. That matches how practice actually flows.

**Two load-bearing invariants (cheap now, expensive to retrofit):**
1. *Grid only where a graded span was declared.* Never infer a grid onto undeclared playing — not onto pulse-less free play, and not merely because a click was audible during warmup. That draws gridlines the drummer never intended and grades them against a fiction. Empty grid is the truthful picture, not a missing feature. This is the closed gimmick door; "grade everything automatically" is the attempt to reopen it.
2. *Grading is derived, never stored.* If a grade is ever written into the file as a fact rather than recomputed from the two tracks, the file drifts out of sync with its own truth and the timeline-is-truth principle quietly dies.

**Boundary of this layer.** These are lenses on *one* timeline. Enrollment/recognition (Layer C) and longitudinal trend (Layer D) are deliberately **not** part of this layer — they are cross-timeline operations on a different axis. Per-performance truth is the piano roll; cross-performance truth ("am I improving over weeks") is a separate trend view — a different question on a different scale. Don't force C or D onto the per-session roll.

*Reconciliation status: §3, §5, §8, and §9 Phase 3 are reconciled to this contract — session = one performance track + an optional sparse grid track. Sounding the metronome and declaring a graded span are **separate acts**: the click may sound in any context and writes nothing; only a declared grade span writes a grid segment. (Corrects an earlier over-collapse that folded "grade this" into the metronome toggle.)*

---

## 2. Engine / interface split (inside the brain)

The analysis is a **pure engine**: records in, results out. No `print`, no `input`, no UI, no file paths baked in. The terminal is one thin caller today; a phone or web front end is a different thin caller of the *same* engine later.

Practical layout:

- `engine/` — pure functions and classes: ingest, grade, infer, match, trend. Deterministic, testable, no side effects.
- `io/` — the `Source` abstraction: `LiveMidiSource` (mido/rtmidi) and `FileSource` (replay a session file). Brain consumes both identically.
- `cli/` — thin terminal interface that calls the engine and prints/plots.
- (later) `app/` or `server/` — a different thin interface over the unchanged engine.

Python is the proving ground. If the app later needs the logic on-device, porting a *validated, self-contained engine* is mechanical translation with the Python version as the reference oracle — not a redesign. The clean engine/interface line is what makes that true.

**DECISION 2a — App runtime later:** Python engine on a small local server the app calls, vs. porting the engine to the app's language. (Defer; the split keeps both open.)

---

## 3. Data model (the contract)

**Event (atom):** `{ t, note, velocity }` — optionally `channel`. `note` is **raw-as-received** kit MIDI and is *always preserved losslessly*; instrument + articulation are *derived views* via the kit profile (§4), and audio replay reads the raw note directly (§4B). Never overwrite raw with a token.

**Control-message atom (raw, preserved):** `{ t, ctrl }`, where `ctrl` is a non-note channel message as received (status/type + data bytes) — hi-hat pedal-position CC, cymbal-choke aftertouch, positional-sensing CCs, etc. It is stored raw and losslessly for the *same reason* `note` is: it is stream information **destroyed if not committed at capture** (the §4.2 assignment test), and the kit-profile `has_positional_sensing` flag and the `HIHAT {open, half, closed}` articulation are *underivable* without it. It is **not** a piano-roll lane — the timeline view (§1A) filters to `event` occurrences — so it is *preserved-but-invisible* until a derivation (HH openness, choke, positional grading) asks for it. This is what makes "raw stream preserved losslessly" true rather than aspirational; the TD-02 may emit little of it, but the *schema* decision is the irreversible part, not any one module's behavior.

**Session:** one continuous performance track plus the metadata raw MIDI doesn't naturally carry. Per §1A, a session is *one performance track + an optional, sparse grid track* — there are no capture modes.

- `schema_version` (integer), `session_id`, `start_iso` (absolute), `kit_profile_id`, `user_id`
- `calibration_offset_ms` (nullable): the audio-out + MIDI-in offset **as measured/configured for *this* setup at capture time** (§6). Recorded per session because on a laptop it changes with audio device, buffer size, and OS state, and cannot be reconstructed later — it is capture-time information by the §4.2 test. Never a global constant; `null` = uncalibrated.
- `grid_track`: list of grid segments `{ start_t, end_t, bpm, grid_subdiv, downbeat_t }`. The app is the **tempo authority**, so each segment's grid is exact, not estimated. Spans not covered by any segment are grid-empty = free play; an empty list = a fully free session. This is a *first-class renderable track* (drawn as gridlines on the roll), which is also why it can't live as buried MIDI tempo meta (→ reinforces 3b).
- `enrollment_spans`: list of `{ start_t, end_t, profile_ref, bpm, grid_subdiv, downbeat_t }` — present only when enrollment was declared; marks which spans demonstrate which groove and **snapshots the running click** (identically to a grade-span declaration) so Layer C can fold reps into beat-relative space. Without the grid params an enrollment span is uninterpretable, so they are part of the atom, not optional. `profile_ref` is a **stable, user-assigned groove identity**, not a pointer to a computed profile — the profile itself is a *rebuildable derived index* (§6, Layer C) that lives in the cross-session store, **not** in the session file (§1A boundary). A declaration that finds **no click sounding is rejected** (nothing to snapshot); this applies to grade-span and enrollment declarations alike.
- `bookmarks`: list of `t`
- `events`: the performance track — `{ t, note, velocity }`

Grading is **not** stored here. It is derived on demand from `events` × `grid_track` (§1A). There is no graded/ungraded flag — a span is gradeable iff a grid segment covers it.

**Serialization — two formats:**

- **Primary: JSON-lines — an append-only log of typed lines.** Line 1 is a `meta` record; every subsequent line is one timestamped occurrence with a `type`: `event` (a performance-track hit), `ctrl` (a raw non-note channel message — the control-message atom above), or a **declaration** (`grid_start` / `grid_end`, `bookmark`, `enroll_start` / `enroll_end`, `session_end`). The writer only ever *appends* — it never edits line 1 — which is exactly what lets the capture half honor its never-block, ring-buffer-to-SD, crash-survivable contract (capture spec §2, §6); grid/bookmark/enrollment state is written *as it is declared, mid-session*, not folded back into meta at close. The **§3 Session object above is the *parsed reduction* of this log, not the file layout**: on read, the engine folds declaration lines into `grid_track` / `enrollment_spans` / `bookmarks`. Crash recovery is therefore free — *truncate to the last complete line*. (This mirrors the gesture grammar of §5/Phase 3: declarations are timestamped occurrences on the timeline, not out-of-band header edits.)
- **Secondary: standard MIDI export.** For DAW interop and because (confirmed against the piano-app market) *an analyzer that ingests arbitrary outside MIDI for timing analysis does not exist* — so emitting MIDI is both interoperability and a small differentiator. The performance track alone maps to SMF trivially; we just don't make SMF carry the grid. *(Export detail, Tier 3 — do not build yet: a grid-empty session takes a fixed default tempo; grid segments map to SMF tempo/time-sig events. One-way and lossy by design.)*

**`t` is monotonic session wall-time (load-bearing invariant).** `t` is session-relative and **never freezes**: the Jamcorder-borrowed auto-pause/resume (capture spec §6) is an *annotation on the timeline*, not a clock stop. IOI and grid math span pauses correctly because `t` keeps counting through them. A pause/resume *may* be recorded as a declaration line for display, but it **never rebases `t`**. (Getting this wrong silently corrupts every inter-onset interval that straddles a pause — cheap to state now, invisible-and-expensive to discover later.)

**DECISION 3a — Event timestamp basis & representation (RESOLVED):** events carry **session-relative integer milliseconds**; meta carries absolute `start_iso`. This gives exact relative timing for grading and absolute dating for longitudinal ordering. Representation is fixed at **integer ms** (`velocity`/`note` integers too): the ~1 ms USB frame floor (capture spec §3) makes finer precision physically meaningless, and integers make the Phase-0 byte-identical round-trip *exact by construction* — a precondition for that round-trip to be a well-defined validation gate across two different producers (laptop host vs. box).

**DECISION 3b — Primary format: JSON-lines, MIDI export-only. The real reason is structural, not preference:** a standard MIDI file (SMF) **fuses time and grid** — it measures time in delta-*ticks relative to an ever-present tempo*. Our §1A contract requires the opposite: a performance track at absolute time plus a **separate, sparse** grid track whose *absence* is meaningful (free play). SMF structurally cannot represent "no grid here," nor the click generator's per-section declared state (grid on/off, BPM, subdivision, phase, changing mid-session) — you'd have to abuse the tempo field as a fake timebase and carry the real semantics out-of-band. JSON stores that state natively. The deciding axis is *not* grading math (trivial in both — in SMF a note's position mod ticks-per-subdivision literally *is* its deviation) but whether the format can hold the generator's declarations. It can't; JSON can. Jamcorder chose SMF-primary correctly *for its problem* — a viewer of always-gridded performances — which is exactly the special case we don't live in.

**Schema evolution (the corpus is permanent).** The log is append-only and lives for years, so it carries a `schema_version` and the reader obeys one fixed policy: *ignore unknown fields; refuse a newer **major** version; accept newer minor versions.* Any change to the **meaning** of an existing field bumps the **major** version — new meaning never silently reuses an old field. Files are never rewritten in place (grading is derived — §1A; the log is append-only — above), so there is no migration-rewrite path to maintain: an old file stays readable under its own version and its derived views are recomputed by the current engine. This one addition is what makes every later schema change (new `ctrl` uses, new articulation, new declaration types) safe rather than corpus-splitting.

---

## 4. Kit / instrument abstraction & normalization

Different e-kits have different note maps, pad counts, and capabilities. The brain normalizes a specific kit's raw MIDI into a canonical form so the same engine works for any drummer. Two things live here: the **vocabulary** (what canonical form is) and **where normalization happens**.

### 4.1 Vocabulary — two axes, not one (4a: RESOLVED)

A hit has two independent properties, and MIDI tangles them into one note number. Keep them separate:

- **Instrument** = the *lane* on the piano roll. A small, **frozen**, cross-kit set, anchored to standard drum notation — the language every drummer and every kit already share (the §1A mental-model test, and the most portable choice if this ever reaches the wider e-drummer market).
- **Articulation** = an **optional attribute** of the event (head/rim, open/closed, bow/bell), populated where the kit sends it, **null where it doesn't**. Additive forever.

Why this split is load-bearing, not tidiness: *splitting an instrument later invalidates stored data* (a year of grooves logged as `SNARE` can't be retroactively separated into head vs. rim), but *adding articulation values is safe*. Making articulation an attribute from day one means you never have to split — you've pre-made every future distinction for free, and a fancier kit just fills the field in while a basic kit leaves it null. That keeps the irreversible part (the lane set) small enough to get right.

**Instruments (lanes, FROZEN):** `KICK, SNARE, HIHAT, HH_PEDAL, TOM_HIGH, TOM_MID, TOM_LOW, RIDE, CRASH, UNMAPPED`. ~10 lanes, matching a standard kit chart. `UNMAPPED` is the catch-all so an unknown note lands on a visible lane instead of vanishing or crashing.

**Articulation (attribute, additive, null-where-absent):** `SNARE {head, rim, crossstick, rimshot}`, `HIHAT {closed, open, half}`, `RIDE {bow, bell, edge}`, `CRASH {bow, edge}`, toms `{head, rim}`. Default = plain hit. Where a kit signals articulation via a *separate channel message* rather than a distinct note — notably `HIHAT {open, half, closed}` from pedal-position CC, and cymbal choke from aftertouch — that value is derived from the **control-message atom** (§3), which is precisely why the `ctrl` atom must be captured raw: without it these fields are permanently null even on kits that send them.

**Specific calls & why:**
- `HH_PEDAL` is its own **instrument**, not a hi-hat articulation — the pedal chick is a *foot* event (a different limb), and feet matter for coordination claims. Open/closed is an articulation of the *hand-played* hat.
- `CRASH` and `RIDE` never merge — different musical function; a ride groove is a different groove than a crash groove, and recognition must see it.
- **Ghost notes are velocity, not articulation** — a ghost is a low-velocity `SNARE head`. Encoding it as a token would double-encode and corrupt velocity metrics.
- Toms are **semantic** (`HIGH/MID/LOW`), not ordinal (`TOM_1..N`) — ordinal breaks cross-kit; a 4+-tom kit adds an *additive* `TOM_4` later, never a resplit. (Trade-off: 4+-tom kits lose a distinction until extended — acceptable; no *claim* needs four toms resolved.)

**Limb-class hint per instrument** (powers coordination claims, honest about the wall): `KICK → foot`, `HH_PEDAL → foot`, everything else → `hands (unresolved)`. Feet are identifiable because they're separate pads; the two hands stay pooled because the data pools them. This is where the **hand-identity wall** (§7) is enforced: the profile maps notes to *pads/instruments*, **never** to `LEFT_HAND`/`RIGHT_HAND` — that information isn't in the stream.

### 4.2 Normalization — brain-side, re-runnable (4b: RESOLVED)

Normalization (raw note → instrument + articulation, via a **kit profile**) happens in the **brain, not the device.** The test that decides every "device or brain?" question: *is the information destroyed if I don't do it now?* Timestamp — yes (the clock is gone the instant the byte lands) → device. Normalization — no (the raw note is preserved) → brain. The device commits only to what can't be reconstructed; the brain re-derives everything that can.

Consequences that make this the whole "works for any drummer" mechanism:
- A fixed or improved profile **retroactively re-normalizes all history for free** — re-run, not re-record. (This is literally the SSD experience: wrong kit profile loaded → playback wrong → swap the profile in config → the same raw MIDI re-maps correctly, recording untouched. The analyzer does to *tokens and grades* what SSD does to *sounds*.)
- Articulation-as-additive only works *because* normalization is re-runnable — extend the vocabulary and old sessions gain the articulation on re-read.
- The **profile registry is brain-owned** — a new kit is a *data* problem (add a profile), never a firmware fork. Capture firmware is identical across all kits.
- Even the optional realtime drift path doesn't pull normalization onto the device — timing-against-grid runs on raw notes and doesn't care whether a pad was rim or head.

**The device emits** raw `(t, note, velocity)` + a `kit_profile_id` — and that id is a **reassignable pointer, not a welded fact.** *Guardrail:* the kit label is the one interpretive bit the device asserts, and it *can* be wrong (wrong profile selected, kit swapped) — so the brain must let you re-point it after the fact, one click, exactly like the SSD config swap. A mislabel is never a re-record. (Unlike a profile *bug*, which re-running fixes globally, a mislabel is only fixable by re-pointing — so it must be re-pointable.)

**Honesty note — "raw" is post-kit-firmware.** The note number and velocity are already *the module's own interpretation*: the kit applied its zone-to-note mapping and its **velocity curve** before the byte left. That's baked in, on the device, unfixably — it happened before capture. So "raw" means *raw as received*, not pristine physics. Practical consequence: cross-kit velocity comparability (Phase 6) is a **brain-side calibration** — approximate, not a guarantee, because two kits with different velocity curves report different MIDI velocities for the same physical hit and that difference is un-removable from stored data. The re-pointable profile layer sits *above* this baked-in layer and can't reach under it.

A kit profile = `{ note → (instrument, articulation) }` map + capability flags (`has_hh_pedal`, `has_positional_sensing`, `velocity_curve_id`). TD-02K is profile #1; its note numbers come from the module's MIDI implementation chart (read, not guessed). Only the *profile* is kit-specific — the vocabulary and the engine are not.

**The registry is versioned (Tier 2, post-Phase-0-safe).** Re-runnability changes historical derived views for free, which is the point — but a derived view, *when surfaced*, carries the `(kit_profile_id, profile_version)` it was computed under. So a re-run after a profile fix is attributable, and a discontinuity in a Layer-D trend is *explainable* ("profile v2 → v3 corrected the ride note") rather than a mystery. Version bumps are additive metadata; they never rewrite session files.

---

## 4B. Audio replay — must-have, at claim-faithful fidelity (4c: RESOLVED)

Audio replay is **not** a viewer nicety; it's part of the measurement loop. A drummer perceives error by *ear* — you hear a hit is off, and that percept is the ground truth the visual grade must agree with. If replay doesn't reproduce what you actually heard, the roll and the sound disagree, and the drummer trusts the ear over the geometry. So audio is what makes the visual grade *credible to the person being graded.* Must-have.

But "how you hear it" splits into two parts with opposite costs:
- **Timing & dynamics** — *when* each hit speaks and *how hard*. This is the part in the grading loop: looseness is *when*, uneven/weak dynamics is *how hard*. Both are carried **losslessly by the raw stream** (onset time + velocity). A plain sample at your recorded velocity and microtiming already sounds loose when you rushed and weak when you ghosted.
- **Timbre / articulation realism** — whether the open hat rings and chokes, the rimshot cracks, the ride washes. This is the **SSD cliff**: stateful instruments, choke modeling, round-robins, deep velocity layers. Months of work — and *not in the grading loop.*

So the fidelity required is **velocity- and microtiming-faithful playback through static, velocity-layered samples** — one shot per instrument per a few velocity layers, **no articulation state machine.** That reproduces exactly the band of "how you hear it" the grade can honestly speak to, and goes vague exactly where the grade goes vague (fine articulation): audio and analysis degrade *together and honestly.* It stops well short of the cliff, because the expensive part (stateful articulation) is precisely the part not in the loop.

- **Perfectionist path (free):** because the raw stream is preserved, "export MIDI → open in your DAW with SSD / your kit module" yields real-kit sound with zero DSP built by us. That's the escape hatch for anyone who wants true timbre.
- **Architectural cost:** this asks the *app* to ship a small static sample set; it asks the *schema* for **nothing new** — replay reads only the preserved raw `(t, note, velocity)`. It is **orthogonal to the *articulation* set, though not to the profile**: replay routes raw note → instrument lane via the *same kit profile* (raw note numbers are kit-specific, so a bare note number can't pick a sample on its own), then lane → static velocity-layered sample, ignoring articulation. Using the one profile — rather than a second, parallel per-kit note→sample map — is what keeps the "swap the profile, playback re-maps" story (§4.2) true for *audio* too, with nothing to drift out of sync. *(Earlier drafts said "bypasses normalization entirely"; that was an overclaim — replay bypasses **articulation**, not the lane mapping.)*

---

## 5. What sits on the raw recording (reconciled with §1A)

Per §1A there are **no capture modes**. There is one capture, always running — the performance track. On top of it the user *declares* things that write to the session, and the system *derives* things on demand. The old ambient-vs-graded split is gone: "ambient" just means *no grid segment here*; "graded" just means *a grid segment here*. Same recording either way.

**Declarations (capture-time, written to the session):**

- **Grade span** — explicitly declaring "grade this, from here to here." Writes a grid segment to `grid_track`, snapshotting the running click's BPM, subdivision, and phase. This is the *only* thing that makes a span gradeable, and it is a **separate act from sounding the metronome** (see below).
- **Enrollment** — tagging a span as a demonstration of a groove (played repeatedly, click running). Writes an `enrollment_spans` entry (snapshotting the click, §3) and feeds the cross-session profile store. The one genuinely distinct operation, because it is *cross-timeline* (§1A boundary), not a lens on this performance.

**Declaration mechanics (v1 rules — resolve the states real practice produces):**
- *Requires a sounding click.* A grade-span or enroll declaration snapshots the running click; **if no click is sounding, the declaration is rejected** (there is nothing to snapshot). This is the data-model meaning of "click running."
- *A click-param change closes the segment.* Changing BPM or subdivision while a grade span is open **closes the current grid segment and opens a new one** at the change point. (v1 may instead simply *disallow* mid-span changes; either is well-defined, "silently keep the stale grid" is not.)
- *Open spans auto-close.* Session end or the idle-timeout failsafe (capture spec §6) **auto-closes** any span left open, at the last event in it — a forgotten "end grade" never produces an unbounded segment.
- *Trigger hits are excluded from analysis.* Gesture patterns are real pad hits, recorded raw and **never discarded** (raw is preserved). But the declaration record carries the trigger-hit span, and the engine **excludes those events by default** from per-pad velocity, IOI, and recognition — so a `kick+crash×2` start gesture neither biases crash-lane stats nor seeds a self-made decoy in Layer C.

**Not a declaration — just an audio feature:**

- **Metronome / click** — the drummer may sound a click in *any* context (warmup, graded, enrollment), at any tempo/subdivision. Sounding it **writes nothing** on its own. This is the crucial separation: you can warm up to a click and *not* be graded. The grid is written only when you also declare a grade span; the declaration snapshots whatever the click is currently set to.

**Derived (no capture-time action):**

- **Grading** — the geometry between the performance track and the grid track, wherever a grade span exists. A review lens, computed on demand, **never shown in-the-moment by default** (the no-anxiety property).
- **Recognition** — passive and brain-side: any enrolled groove is matched in later performances automatically (Layer C). Not a capture-time mode.

**Concretely:**

- *Free play* = a span with no grid segment — including a click-accompanied warmup you didn't declare graded. The *no-friction view* = a session whose grid track is empty — just notes on the axis, scrub, listen, bookmark.
- *Integrity invariant (§1A):* a grid segment is written only where a grade span was declared — never inferred, and never merely because the click was audible.
- Claim boundaries (what may / must-not be asserted) live in §7 — they're a property of the *information*, not of a "mode."
- *Out of scope:* curriculum/teaching (owned by Melodics et al.). This system grades *your own* material; it doesn't deliver lessons.

---

## 6. Metric layers (the depth ladder)

**Layer A — reference-free arithmetic (the honest floor).**
Per-event deviation from the click grid; mean signed offset = rush/drag, bucketed **per BPM**; std of offset = consistency; per-pad velocity mean/variance; inter-onset interval distribution. Always valid, needs no intent. This is "just" subtraction against a known grid — and it's the useful core.
`grid_tick_ms = 60000 / bpm / grid_subdiv`; for each event, nearest grid line → signed deviation. A **calibration offset** (audio-out buffer + MIDI-in latency) is subtracted — read **per session** from meta `calibration_offset_ms` (§3), *not* a single global constant: it is constant for a fixed setup but shifts across setups (audio device, buffer size, OS), so a laptop that changes output between sessions would otherwise inject a step change straight into the Layer-D rush/drag trend.

**Layer D — longitudinal statistics (most useful, least flashy).**
Is the session-over-session change *real* or just variance? Per metric, per groove, per BPM. Sits on Layer A alone — needs no inference. This is the layer that actually answers *"am I getting better or just used to feeling bad."* Report trend with an uncertainty band; do not overclaim significance at small n. *(Tier 3 — do not build yet: if recompute-over-corpus ever gets slow, a derived-metric cache keyed by `(schema_version, engine_version, profile_version)` is permitted as a **rebuildable, non-authoritative** index. Do **not** "fix" performance by writing grades into the file — that reopens exactly the door §1A closed.)*

**Layer B — structure inference (the seam: arithmetic → ML).**
Subdivision inferred from onset spacing (IOI histogram in beat-relative units; binary /2/4/8/16 vs ternary /3/6/12). When playing is regular, spacing clusters → high confidence; when sparse/mixed → low confidence → suppress the claim or fall back to declared. Every Layer-A metric is then recomputed against the *inferred* grid and tagged with confidence. This is what makes a consistency number correct on non-trivial input instead of a confident lie.

**Layer C — groove enrollment / recognition (the star).**
- *Enroll* (with click): play groove ×N reps; fold reps onto one bar in beat-relative space; per grid-slot compute P(onset on pad), mean velocity, mean micro-timing, and variances. Result = **profile** = skeleton (which slots) + rendering stats (how you play them), cleanly separated *because* enrollment used the click.
- *Store* beat-relative → tempo-invariant. The stored profile is a **rebuildable derived index** over `enrollment_spans` (which live in session files), carrying provenance — source sessions/spans + profile & engine version. It is **never a second source of truth**: fix a kit profile (re-normalization) or switch the matcher (DTW→embeddings) and the profiles **recompute** from the enrollment spans, exactly as grades recompute from the two tracks. This keeps the derived-never-stored invariant from quietly dying at the profile store's door — the one place §1A's rule could otherwise leak.
- *Recognize*: sliding window over a stream; recover tempo from the groove's **own periodicity** (autocorrelation — no click needed); fold candidate to beat-relative; distance to each profile; threshold → match / **no-match**. The rejection ("none of the above") boundary is the hard part and the key validation metric.
- *Stats*: on matched occurrences, compute Layer A/B metrics, report **tempo-conditionally**, feed Layer-D trend per groove.
- *Difficulty dial*: v1 DTW templates → v2 learned groove embeddings (metric learning — literally how face recognition works under the hood).

**Recommended build order: A → D → B → C.** Each stop ships something honest; the hardest, most career-relevant rung (C) is last, where it belongs. You can't fully fail.

---

## 7. Hard scope boundaries (baked in, the engine must *refuse*)

**Claimable:** timing & velocity per pad; drift; rush/drag; foot-vs-foot and hands-vs-feet coordination (separate pads); subdivision on regular patterns; longitudinal trend of all the above.

**Unclaimable — engine refuses rather than fakes:**
- **Hand-vs-hand identity** — tier-3 sensor wall. Not in pad-indexed MIDI; no declaration or inference recovers it. Only a new sensor (wrist/stick IMU) could. (One hairline exception: onsets spaced tighter than one limb can physically produce imply *two limbs*, never *which*.)
- **Expressive fills graded metronomically** — measures the wrong thing.

**The lane** (confirmed against the entire piano-app market, which is far more crowded): analysis of your *own free practice on your own material*. The piano apps all grade *their* declared lessons in *their* walls and don't ingest outside performances. That empty quadrant is the product.

---

## 8. What the brain asks of the capture device

This is the reverse-dependency map: each brain feature imposes a requirement on the hardware. The capture device's whole feature set is *derived* from the brain, not designed independently.

| Brain feature | Requirement it places on the capture device |
|---|---|
| Layer A grading | Arrival-edge timestamping; click generated by device (tempo authority) so a declared grid is exact; grid params written to session on grade-span declaration |
| Recording (always on) | Continuous capture; session metadata; silent logging (no realtime score on device) |
| Metronome / click | Click generation, tempo authority (BPM + subdivision). Sounding the click writes nothing on its own. |
| Grade-span declaration | A control input to start/end a graded span; on declaration it snapshots the running click's BPM/subdivision/phase into a grid segment. **Separate from sounding the metronome** — warmup-with-click is not graded. |
| Grading (derived) | Nothing beyond the grade span's grid — derived from `events` × `grid_track` (§1A/§5). |
| Enrollment | Control input to signal enroll-start / enroll-stop; click present during enrollment |
| Recognition | Persistent storage of enrolled **profiles**; otherwise brain-side (no new hardware) |
| Longitudinal stats (D) | Persistent session storage + **session dating/ordering** (RTC or host-provided time); reliable session boundaries |
| Session segmentation | Gesture-based start/stop + **idle-timeout failsafe** for session end |
| Normalization | **Brain-side, not device.** Device emits raw-as-received `(t, note, velocity)` **and raw non-note channel messages (`ctrl` — pedal position, choke, positional CC)** + a **reassignable** `kit_profile_id` pointer; brain owns the profile registry and re-derives instrument/articulation on read. New kit = add a profile (data), not a firmware change. |
| Calibration (Layer A) | Host/device measures the audio-out + MIDI-in offset for the current setup and records it as **per-session** `calibration_offset_ms` (§6); the engine subtracts it per session, never as a global constant. |
| Multi-drummer | Kit-agnostic raw passthrough (per above); per-user profile storage; cross-kit velocity comparability is a brain-side calibration (raw is post-kit-firmware). |

**Derived capture-device feature spec:**
1. Accurate arrival-edge timestamping (shared clock with the click).
2. Click generation / metronome (device/app is tempo authority; BPM + subdivision). Sounding it writes nothing; a grade-span declaration snapshots it into the grid.
3. Control-input channel — the **gesture grammar** (see Phase 3): e.g. kick+crash×2 = session start/stop, top-pad gesture = bookmark, a grade-span start/end gesture, an enroll gesture. Distinct from the metronome control (which sounds the click and sets BPM/subdivision but writes nothing until a grade span is declared).
4. Session boundary logic: gesture start + idle-timeout failsafe.
5. Persistent storage + session dating.
6. Kit-agnostic raw passthrough + a reassignable `kit_profile_id` tag; per-user/profile storage. (Normalization itself is brain-side — §4.2.)

(Note: in the laptop phase, gesture detection and dating live in the brain/host; in the standalone box they migrate into firmware. That migration is a **capture/brain boundary decision** — DECISION 9c.)

---

## 9. Development phases

Each phase ships something usable and states: brain work, what it asks of capture, validation/adjudication, open decisions.

### Phase 0 — Foundation & contract
- **Brain:** lock the schema (§3) — including `schema_version` + reader policy, the **append-only typed-line log** (Session = its parsed reduction), the `ctrl` control-message atom, per-session `calibration_offset_ms`, the enrollment-span grid snapshot, and the integer-ms/`0–127` representation; implement kit profile (§4) for TD-02K; scaffold engine/io/cli split; `LiveMidiSource` via mido + `FileSource` replay. Own the **laptop-phase capture half** (host is tempo authority, one clock — capture spec §4A).
- **Asks of capture:** none new — source is kit→Reaper/mido, but the host process (not Reaper) generates the grading click and stamps MIDI on one clock (capture spec §4A).
- **Validation:** round-trip — capture a session, replay from file, **byte-identical** event stream (now well-defined given fixed representation); crash-recovery = truncate-to-last-complete-line leaves a valid session.
- **Decisions:** 3a **resolved** (integer ms; §3), 3b (log layout; §3), 4a (token set).
- **Ships:** the jamcorder-equivalent recorder, on the laptop.

### Phase 1 — Layer A: reference-free floor
- **Brain:** click-grid grading; rush/drag per BPM; consistency; per-pad velocity; IOI; constant-offset calibration routine.
- **Asks of capture:** arrival-edge timestamp; click as tempo authority; BPM in meta.
- **Validation / adjudication:** record an intentionally metronomic take and an intentionally degraded take → metrics must rank them correctly. Compare against a parallel DAW capture for timestamp ground-truth (±tolerance). **Drummer check:** the rush/drag number matches felt sense.
- **Decisions:** BPM bucket boundaries.
- **Ships:** objective timing feedback on real playing — the honest core.

### Phase 2 — Layer D: longitudinal stats
- **Brain:** append-only session store indexed by date (the store *is* the corpus of §3 logs — one file per session/day, dated from meta; no new format); per-metric time series; trend-vs-variance (effect size / confidence band; tempo as covariate → trend per BPM). Per-session `calibration_offset_ms` is subtracted before any cross-session comparison, so a setup change never masquerades as a trend.
- **Asks of capture:** persistent storage; session dating; reliable boundaries.
- **Validation:** ≥5 sessions; confirm a real improvement registers and pure noise does not.
- **Decisions:** stat method (CI/effect-size vs regression); how to present uncertainty so it's not over-read.
- **Ships:** the "am I actually getting better" answer — the defensible lane.

### Phase 3 — Declarations & control surface
- **Brain:** the declaration surface from §5 — grade-span declarations (which write grid segments, snapshotting the running click) and enrollment spans; a gesture-grammar state machine over the event stream (session start/stop, bookmark, grade-span start/end, enroll-start/stop). No modes. Sounding the metronome is a *separate* act that writes nothing.
- **Asks of capture:** the control-input channel (gestures) and the metronome/tempo control (which sounds the click and sets BPM/subdivision but writes nothing); the grade-span declaration is what writes the grid; idle-timeout failsafe.
- **Validation:** gestures fire reliably and are not triggered by normal playing (false-trigger rate ≈ 0); a click-accompanied warmup with no grade declaration produces no grid and no grade.
- **Decisions:** **9c — where gesture detection lives** (brain/host now vs. firmware in the standalone box); the exact gesture vocabulary (drummer-friendly, un-playable-by-accident).
- **Ships:** the no-anxiety practice loop — play freely (even to a click), declare a graded span only when you want one, review later.

### Phase 4 — Layer B: structure inference
- **Brain:** subdivision inference from IOI clustering; grid-snapping under uncertainty with confidence; recompute Layer-A metrics against inferred grid; suppress low-confidence claims.
- **Asks of capture:** *nothing new* — pure brain (proves the layering is clean).
- **Validation:** 8ths / triplets / 16ths / mixed → subdivision detected; consistency stays correct where naive nearest-click would lie.
- **Decisions:** heuristic (IOI clustering) vs light model; confidence threshold for suppression.
- **Ships:** trustworthy numbers on non-trivial playing.

### Phase 5 — Layer C: enrollment & recognition (the star)
- **Brain:** enrollment convergence (beat-relative profile = skeleton + rendering stats); profile storage **as a rebuildable, provenance-tagged derived index over `enrollment_spans`** (§6 — recomputes on re-normalization or matcher change, never a second source of truth); periodicity-based tempo recovery; query-by-example spotting; rejection threshold; per-groove tempo-conditional stats → Layer-D trend per groove. v1 DTW.
- **Asks of capture:** profile storage; otherwise brain-side.
- **Validation:** enroll a groove; replay sessions containing it + decoys; measure detection rate **and false-positive rate** (rejection boundary is the headline metric); confidence surfaced.
- **Decisions:** DTW vs embeddings and the jump point; rejection threshold calibration; min reps to converge; window/segmentation strategy.
- **Ships:** zero-friction longitudinal tracking of your own grooves — and the genuine ML niche probe.

### Phase 6 — Multi-drummer generalization
- **Brain:** kit profile registry (common kits + capability flags); per-user/kit calibration (timing offset, velocity normalization, HH handling); per-user data isolation.
- **Asks of capture:** kit-agnostic passthrough; per-user storage.
- **Validation / adjudication:** run the whole pipeline on ≥1 *other* drummer + *different* kit; metrics must hold up and match that drummer's felt sense. This is where "adjudicated by other drummers" becomes a real test, not a claim.
- **Decisions:** canonical token set finalization; auto vs manual calibration; how kit profiles get expanded/contributed.
- **Ships:** usable beyond you.

### Phase 7 — Vision horizon (keep, don't build yet)
- **Coach / prescription layer:** turn observer output into prescriptions (weakness → targeted drill), closing the deliberate-practice loop. Integrity rule: only prescribe *confidently* where measurement is *reliable* — never launder a foggy inference into authoritative advice.
- **Phone app:** new thin interface over the unchanged engine; optionally push only the cheap Layer-A arithmetic near-realtime for a live drift indicator — which must be **another thin consumer of the identical event stream**, not a parallel capture/analysis path (that would fork the engine and reintroduce in-the-moment scoring on the wrong side of the split).
- **Content / portfolio:** the build-log itself as the artifact — the three-act arc (problem → observer → coach) is the same as the phase progression.

---

## 10. Adjudication — the two readers

The system has two audiences whose trust it must earn, and they stress different parts:

- **Drummer-you:** metrics must be *actionable and trustworthy* — match your felt sense, point at something you can work on. Earned by Phases 1–5 validation against your own controlled recordings.
- **Other e-drummers:** the same claims must survive *different kits, different players, different setups*. Earned by Phase 6 — kit abstraction, calibration, and cross-drummer validation.

Validation is therefore not a final step; it's a per-phase obligation (metronomic-vs-degraded ranking, DAW ground-truth, ≥5-session trend reality, detection/false-positive rates, cross-drummer replication). A claim the engine can't validate is a claim the engine must refuse (§7).

**Standing fixture corpus (Tier 2, cheap because the engine is pure).** These per-phase validations are not one-time gates — the recordings that pass them are **checked into the repo as versioned golden fixtures**, alongside a few *synthetic* sessions whose metrics are analytically known. Because the engine is pure (§2), a golden-file regression is nearly free, and every later refactor (Layer-B recompute, a profile re-run, the DTW→embedding switch) is guarded against a silent 2 ms drift in numbers that already shipped. The validation gate a phase passes *becomes* its permanent regression test.

---

## 11. Consolidated decision register

| ID | Decision | Status / call |
|---|---|---|
| 2a | App runtime later | Deferred; engine/interface split keeps it open |
| 3a | Event timestamp basis & representation | **Resolved:** session-relative **integer ms** in events (`velocity`/`note` int), absolute `start_iso` in meta (§3) |
| 3b | Primary serialization | **Resolved:** JSON-lines primary as an **append-only typed-line log** (Session = its parsed reduction), MIDI export-only (SMF fuses time+grid — §3) |
| 3c | Schema evolution | **Resolved:** `schema_version` + reader policy (ignore unknown fields, refuse newer major); files never rewritten (§3) |
| 3d | Control-message atom | **Resolved:** raw `ctrl` line captured losslessly; HH openness/choke/positional derived from it (§3/§4.1) |
| 3e | Calibration offset | **Resolved:** per-session `calibration_offset_ms` in meta, subtracted per session (§3/§6) |
| 3f | Enrollment span grid | **Resolved:** enroll declaration snapshots the click (`bpm/subdiv/downbeat`); declarations require a sounding click (§3/§5) |
| 4a | Instrument vocabulary | **Resolved:** ~10 notation lanes frozen; articulation = additive attribute (§4.1) |
| 4b | Normalization location | **Resolved:** brain-side, re-runnable; device emits raw + reassignable `kit_profile_id` (§4.2) |
| 4c | Audio replay fidelity | **Resolved:** must-have; velocity/microtiming-faithful, static samples, no articulation engine; Tier-3 export for true timbre (§4B) |
| 9c | Gesture detection location | Brain/host now; migrate to firmware for standalone box |
| — | Layer B approach | Heuristic IOI clustering first |
| — | Layer C matcher | DTW first; embeddings when the work proves it wants you |
| — | Build order | A → D → B → C |

---

---

## 12. Design-review integration log (pre-implementation review)

Integrated from a principal-architect design review conducted before writing code. Verdict was **B — ready after fixing Tier 1**; all six Tier 1 items are now in the contract above. Traceability:

**Tier 1 (in the contract, Phase 0):**
1. *Schema versioning & evolution policy* — §3 (`schema_version`, reader policy; files never rewritten).
2. *Serialization = append-only typed-line log; Session is its parsed reduction; `t` monotonic across auto-pause* — §3.
3. *Control-message atom (`ctrl`) so "raw preserved losslessly" is true; HH/choke/positional derivable* — §3, §4.1, §8.
4. *Laptop-phase capture half owns the one-clock rule (host is tempo authority, not Reaper)* — capture spec §4A + Phase 0.
5. *Calibration offset is per-session, not a global constant* — §3 meta, §6, §8.
6. *Enrollment spans snapshot the click; declarations require a sounding click* — §3, §5.

**Tier 2 (integrated, none forced ahead of its phase):** grade-span edge semantics + auto-close + trigger-hit exclusion (§5); kit-profile registry versioning (§4.2); profile store as rebuildable derived index (§6, Phase 5); audio-replay orthogonality corrected to articulation-not-profile (§4B); numeric representation fixed (§3/3a); standing fixture corpus (§10).

**Tier 3 (noted, explicitly *do not build yet*):** derived-metric caching (§6 Layer D); SMF export details (§3); live drift indicator constrained to the shared stream (Phase 7); session-store bound to the log format (Phase 2).

Invariants preserved unchanged: engine purity; performance-track/grid-track split; JSON canonical / MIDI export; optional grading; free-and-graded coexistence; statistics on canonical events; incremental roadmap; DTW-first; simplicity over generality.

---

*End of spec. Edit freely as decisions resolve — this is the contract both halves of the project hang off.*
