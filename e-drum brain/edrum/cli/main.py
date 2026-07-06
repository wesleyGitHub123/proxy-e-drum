"""S8 — thin CLI (brain spec §2). Wiring and printing only.

Commands:
    edrum ports                    list MIDI input ports (needs [live] extra)
    edrum record                   capture a session from a live port
    edrum replay FILE              round-trip byte-identity check (Phase 0 gate)
    edrum dump FILE [--normalized] inspect a session file

Any conditional more interesting than argument handling belongs in engine/ or
io/ — keeping this caller thin is what makes a future app a drop-in second
caller of the unchanged engine.
"""

from __future__ import annotations

import argparse
import sys
from datetime import datetime
from pathlib import Path

from edrum.engine.normalize import normalize_event
from edrum.engine.profiles import ProfileError, load_profile
from edrum.engine.records import EventRecord, to_line
from edrum.engine.session import reduce_session
from edrum.io.capture import run_capture
from edrum.io.clock import SessionClock
from edrum.io.logfile import SessionWriter, read_log, session_filename
from edrum.io.sources import FileSource, LiveMidiSource


def default_profiles_dir() -> Path:
    """The repo's profiles/ directory (editable install / source checkout)."""
    return Path(__file__).resolve().parents[2] / "profiles"


def _load_profile_by_id(profile_id: str, profiles_dir: Path):
    path = profiles_dir / f"{profile_id}.json"
    if not path.is_file():
        raise ProfileError(
            f"no profile {profile_id!r} in {profiles_dir} (use --profiles-dir to point elsewhere)"
        )
    return load_profile(path)


# ---------------------------------------------------------------------------
# commands
# ---------------------------------------------------------------------------

def cmd_ports(_args) -> int:
    try:
        import mido

        names = mido.get_input_names()
    except Exception as exc:  # missing backend, etc.
        print(f"error: cannot list MIDI inputs ({exc}). Is the [live] extra installed?")
        return 1
    if not names:
        print("no MIDI input ports found (is the kit connected and powered?)")
        return 1
    for name in names:
        print(name)
    return 0


def cmd_record(args) -> int:
    clock = SessionClock()

    port_name = args.port
    if port_name is None:
        try:
            import mido

            names = mido.get_input_names()
        except Exception as exc:
            print(f"error: cannot list MIDI inputs ({exc}). Is the [live] extra installed?")
            return 1
        if len(names) == 1:
            port_name = names[0]
            print(f"using the only input port: {port_name}")
        else:
            print("error: specify --port; available inputs:")
            for name in names or ["(none)"]:
                print(f"  {name}")
            return 1

    source = LiveMidiSource(
        port_name,
        clock,
        kit_profile_id=args.kit,
        user_id=args.user,
        calibration_offset_ms=args.calibration,
    )
    try:
        meta = source.open()
    except Exception as exc:
        print(f"error: cannot open MIDI input {port_name!r}: {exc}")
        return 1

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / session_filename(datetime.fromisoformat(meta.start_iso), meta.session_id)
    writer = SessionWriter(path, meta)

    def echo(record) -> None:
        if not args.quiet and isinstance(record, EventRecord):
            print(f"  t={record.t}ms note={record.note} vel={record.velocity}")

    print(f"recording to {path}")
    print("Ctrl+C to stop")
    count = 0
    try:
        count = run_capture(source, writer, on_record=echo)
    except KeyboardInterrupt:
        source.stop()
        # drain anything stamped before the interrupt (never drop an event)
        for record in source.events():
            writer.append(record)
            count += 1
    finally:
        end_t = clock.now_ms()
        writer.close(end_t)
        source.close()

    print(f"session closed: {count} record(s), {writer.last_t} ms, {path}")
    return 0


def cmd_replay(args) -> int:
    """The Phase 0 validation gate: parse, re-serialize, byte-compare."""
    path = Path(args.file)
    lr = read_log(path)
    reconstructed = to_line(lr.meta) + b"".join(to_line(r) for r in lr.records)
    original = path.read_bytes()
    complete = original[: len(original) - lr.recovery.dropped_bytes]

    if lr.recovery.truncated:
        print(f"note: crash-recovered; dropped {lr.recovery.dropped_bytes} torn tail byte(s)")
    if not lr.recovery.has_session_end:
        print("note: no session_end (unclean stop) - session still valid")

    session = reduce_session(lr.meta, lr.records)  # exercises the fold too
    for warning in session.warnings:
        print(f"warning: {warning}")

    if reconstructed == complete:
        print(
            f"PASS byte-identical round-trip: {len(lr.records)} record(s), "
            f"{len(session.events)} event(s), {len(session.grid_track)} grid segment(s)"
        )
        return 0
    print("FAIL: re-serialization differs from file bytes")
    return 1


def cmd_dump(args) -> int:
    path = Path(args.file)
    lr = read_log(path)
    session = reduce_session(lr.meta, lr.records)

    meta = session.meta
    print(f"session   {meta.session_id}")
    print(f"start     {meta.start_iso}")
    print(f"kit       {meta.kit_profile_id}   user {meta.user_id}")
    print(f"calibration_offset_ms {meta.calibration_offset_ms}")
    print(
        f"records   {len(lr.records)} | events {len(session.events)} | ctrl {len(session.ctrl)}"
        f" | end_t {session.end_t}"
    )
    if lr.recovery.truncated:
        print(f"recovery  dropped {lr.recovery.dropped_bytes} torn tail byte(s)")
    for seg in session.grid_track:
        print(
            f"grid      [{seg.start_t}..{seg.end_t}] bpm={seg.bpm} subdiv={seg.subdiv}"
            f" downbeat_t={seg.downbeat_t}"
        )
    for span in session.enrollment_spans:
        print(f"enroll    [{span.start_t}..{span.end_t}] {span.profile_ref!r} bpm={span.bpm}")
    for t in session.bookmarks:
        print(f"bookmark  t={t}")
    for warning in session.warnings:
        print(f"warning   {warning}")

    profile = None
    if args.normalized:
        profile_id = meta.kit_profile_id
        if profile_id is None:
            print("error: --normalized needs a kit_profile_id in meta")
            return 1
        try:
            profile = _load_profile_by_id(profile_id, Path(args.profiles_dir))
        except ProfileError as exc:
            print(f"error: {exc}")
            return 1
        print(f"profile   {profile.profile_id} v{profile.profile_version} ({profile.name})")

    limit = args.limit if args.limit is not None else len(session.events)
    for event in session.events[:limit]:
        line = f"  t={event.t:>8} note={event.note:>3} vel={event.velocity:>3}"
        if profile is not None:
            view = normalize_event(event, profile)
            art = f".{view.articulation}" if view.articulation else ""
            line += f"  {view.instrument.value}{art} [{view.limb.value}]"
        print(line)
    if limit < len(session.events):
        print(f"  ... {len(session.events) - limit} more event(s)")
    return 0


# ---------------------------------------------------------------------------
# argument parsing
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="edrum", description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("ports", help="list MIDI input ports").set_defaults(func=cmd_ports)

    p_rec = sub.add_parser("record", help="record a session from a live MIDI port")
    p_rec.add_argument("--port", help="MIDI input port name (default: the only one)")
    p_rec.add_argument("--kit", default="td02k", help="kit_profile_id tag (default: td02k)")
    p_rec.add_argument("--user", default="local", help="user_id (default: local)")
    p_rec.add_argument(
        "--calibration",
        type=int,
        default=None,
        help="calibration_offset_ms for this setup (default: null = uncalibrated)",
    )
    p_rec.add_argument("--out", default="sessions", help="output directory (default: sessions)")
    p_rec.add_argument("--quiet", action="store_true", help="do not echo hits")
    p_rec.set_defaults(func=cmd_record)

    p_rep = sub.add_parser("replay", help="round-trip byte-identity check")
    p_rep.add_argument("file")
    p_rep.set_defaults(func=cmd_replay)

    p_dump = sub.add_parser("dump", help="inspect a session file")
    p_dump.add_argument("file")
    p_dump.add_argument("--normalized", action="store_true", help="derive lanes via kit profile")
    p_dump.add_argument(
        "--profiles-dir", default=str(default_profiles_dir()), help="profile registry directory"
    )
    p_dump.add_argument("--limit", type=int, default=None, help="max events to print")
    p_dump.set_defaults(func=cmd_dump)

    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
