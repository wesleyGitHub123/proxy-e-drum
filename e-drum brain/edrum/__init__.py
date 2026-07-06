"""edrum — the brain half of the e-drum practice tool.

Two-machine split (brain spec §1): this package is everything *except* capture.
It consumes session files (or a live MIDI port wearing the capture hat,
capture spec §4A) and derives all analysis from them, revisably, forever.

Package layout (brain spec §2):
    engine/  pure functions and data — no I/O, no UI, no file paths baked in
    io/      the Source abstraction, session-log reader/writer, clocks, capture
    cli/     thin terminal caller
"""

__version__ = "0.1.0"
