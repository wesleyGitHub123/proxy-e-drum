# TD-02 VBUS Enumeration Experiment

Purpose: answer one narrow question with the smallest possible firmware spike: does the Roland TD-02 enumerate as a USB MIDI device on an ESP32-S3 USB host when VBUS is absent, or does it require ~5 V present on VBUS first?

## Hypothesis

The TD-02 will not appear as a USB device unless the host port presents valid VBUS first. If that is true, the experiment should show zero `NEW_DEV` events and no descriptor reads in the no-VBUS case, then a normal attach + descriptor sequence once VBUS is injected.

## Minimal setup

1. ESP32-S3 DevKitC-1 running the host probe, preserved at
   [tools/probe/usb_enumeration_probe.cpp](tools/probe/usb_enumeration_probe.cpp)
   (src/main.cpp has since become the production composition root).
2. Roland TD-02.
3. The intended USB cable/adapter path.
4. A way to supply or remove 5 V on VBUS without changing D+/D- wiring.
5. Serial monitor for the log stream.

## Test matrix

1. TD-02 powered, VBUS absent.
2. TD-02 powered, VBUS present.
3. Replug with VBUS still present.
4. Power-cycle the TD-02 with VBUS still present.

## What the firmware logs

The probe prints a timestamped line for each stage:

1. USB host install.
2. USB client registration.
3. `NEW_DEV` / `DEV_GONE` / suspend / resume events.
4. Device descriptor read success or failure.
5. Active configuration descriptor read success or failure.
6. Interface discovery, with a specific line when it sees class `0x01` / subclass `0x03`.

## Success criteria

The hypothesis is supported if the TD-02 never appears without VBUS and consistently appears with VBUS present, with a descriptor sequence that reaches the MIDI streaming interface.

## Failure criteria

The hypothesis is weakened if the TD-02 appears without VBUS, or if the device never enumerates even with stable VBUS and a known-good cable path.

## Execution steps

1. Flash the probe.
2. Open the serial console at 115200.
3. Boot once with VBUS absent and record the log.
4. Re-run with VBUS injected and record the log.
5. Repeat the attach/detach cycle at least three times.

## Data collection template

| Run | Board build | Cable / adapter | VBUS state | Meter reading | `NEW_DEV` seen | Device descriptor read | Config descriptor read | MIDI interface seen | Notes |
|---|---|---|---|---|---|---|---|---|---|
| 1 |  |  | absent |  |  |  |  |  |  |
| 2 |  |  | present |  |  |  |  |  |  |
| 3 |  |  | present |  |  |  |  |  |  |

## ADR template

### Context

We need to know whether the TD-02 requires VBUS before USB MIDI enumeration on the ESP32-S3 host path.

### Decision

Record one of these outcomes:

1. TD-02 requires externally supplied VBUS.
2. TD-02 enumerates without VBUS in this topology.
3. Result is inconclusive; cable or power path must be reworked.

### Evidence

Link the log excerpt, meter reading, and cable/topology used.

### Consequences

State whether the final enclosure must include powered VBUS injection, whether the host path is viable, and whether the broader firmware architecture remains unchanged.