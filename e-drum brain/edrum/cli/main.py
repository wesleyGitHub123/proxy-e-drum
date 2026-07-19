"""S8 — thin CLI (brain spec §2). Wiring and printing only.

Commands:
    edrum ports                    list MIDI input ports (needs [live] extra)
    edrum record                   capture a session from a live port
    edrum replay FILE              round-trip byte-identity check (Phase 0 gate)
    edrum dump FILE [--normalized] inspect a session file
    edrum analyze FILE [--json]    Layer A analysis: timing/velocity/IOI
                                   (phase1-stats-plan; results derived, never stored)
    edrum sync --port COMx         pull session files off the capture box
                                   (needs [device] extra; capture spec §13)
    edrum rm --port COMx NAME      delete one closed session off the capture
                                   box's SD card (host-requested only; §13.1)

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
        # Display naming is a presentation-layer concern (architecture
        # review): the capture log never invents a name, so an unlabeled
        # span shows a placeholder here rather than `None`.
        label = span.profile_ref if span.profile_ref is not None else "(unlabeled)"
        print(f"enroll    [{span.start_t}..{span.end_t}] {label!r} bpm={span.bpm}")
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


def _scope_str(scope) -> str:
    parts = []
    if scope.span is not None:
        tag = "" if scope.span.kind == "grid" else f" ({scope.span.kind})"
        parts.append(f"span[{scope.span.start_t}..{scope.span.end_t}]{tag}")
    if scope.bpm is not None:
        parts.append(f"bpm={scope.bpm}")
    if scope.subdiv is not None:
        parts.append(f"subdiv={scope.subdiv}")
    if scope.lane is not None:
        art = f".{scope.articulation}" if scope.articulation else ""
        parts.append(f"lane={scope.lane}{art}")
    if scope.note is not None:
        parts.append(f"note={scope.note}")
    if scope.grid_source is not None:
        parts.append(f"grid={scope.grid_source}")
    return " ".join(parts) if parts else "session"


def _value_str(value, unit: str) -> str:
    if value is None:
        return "n/a"
    if isinstance(value, float):
        return f"{value:+.2f} {unit}" if unit == "ms" else f"{value:.2f}"
    if isinstance(value, tuple):
        return f"[{len(value)} rows]"
    if hasattr(value, "__dataclass_fields__"):  # e.g. MeanStd
        return " ".join(
            f"{name}={_value_str(getattr(value, name), unit)}"
            for name in value.__dataclass_fields__
        )
    return str(value)


def _quality_str(q) -> str:
    parts = [f"n={q.n}"]
    if q.n_excluded:
        parts.append(f"excluded={q.n_excluded}")
    if q.n_ambiguous:
        parts.append(f"ambiguous={q.n_ambiguous}")
    if q.confidence is not None:
        parts.append(f"confidence={q.confidence:.2f}")
    if q.flags:
        parts.append(",".join(q.flags))
    return " ".join(parts)


def cmd_analyze(args) -> int:
    """Thin caller for the analysis pipeline (engine/analysis.py). All
    rendering here is generic over the result envelope — a newly registered
    analyzer shows up with zero CLI changes (micro-decision 19)."""
    import json as _json

    from edrum.engine.analysis import report_to_dict, run_analyses

    path = Path(args.file)
    lr = read_log(path)
    session = reduce_session(lr.meta, lr.records)

    analyzers = None
    if args.metrics is not None:
        from edrum.engine.analyzers import by_id

        analyzers = []
        for metric_id in args.metrics.split(","):
            analyzer = by_id(metric_id.strip())
            if analyzer is None:
                from edrum.engine.analyzers import ALL

                print(f"error: unknown metric {metric_id.strip()!r}; available:")
                for a in ALL:
                    print(f"  {a.id}")
                return 1
            analyzers.append(analyzer)

    # Profile resolution: --kit is the re-pointable override (spec §4.2);
    # otherwise follow the meta pointer. No profile is not an error — the
    # profile-dependent analyzers refuse honestly instead.
    profile = None
    profile_warn = None
    profile_id = args.kit or session.meta.kit_profile_id
    if profile_id is not None:
        try:
            profile = _load_profile_by_id(profile_id, Path(args.profiles_dir))
        except ProfileError as exc:
            if args.kit:
                print(f"error: {exc}")
                return 1
            profile_warn = f"profile {profile_id!r} not loadable ({exc}); lane views unavailable"

    report = run_analyses(session, profile=profile, analyzers=analyzers)

    if args.json:
        print(_json.dumps(report_to_dict(report), indent=2))
        return 0

    print(f"session   {report.session_id}")
    if report.profile_id is not None:
        print(f"profile   {report.profile_id} v{report.profile_version}")
    cal = report.calibration_offset_ms
    print(f"calibration_offset_ms {cal}" + ("" if report.calibrated else "  (uncalibrated)"))
    if profile_warn:
        print(f"warning   {profile_warn}")
    for warning in report.warnings:
        print(f"warning   {warning}")
    for refusal in report.refusals:
        print(f"refused   {refusal.analyzer_id}: {refusal.reason}"
              + (f" ({refusal.detail})" if refusal.detail else ""))

    current_id = None
    for r in report.results:
        if r.analyzer_id != current_id:
            current_id = r.analyzer_id
            print(f"{current_id} (v{r.analyzer_version})")
        print(f"  {_scope_str(r.scope):<44} {_value_str(r.value, r.unit):<28}"
              f" {_quality_str(r.quality)}")
    return 0


def cmd_sync(args) -> int:
    """Pull the capture box's session archive over the device link
    (capture spec §13). Byte-exact replication: a synced file is
    indistinguishable from a card-reader copy, and the engine never knows
    this command exists."""
    try:
        from edrum.io.devlink import (
            DeviceClient,
            DeviceError,
            DeviceStorageError,
            SerialByteLink,
            check_schema_compatible,
            sync_sessions,
            wake_console_handoff,
        )
    except ImportError as exc:
        print(f"error: device link unavailable ({exc}). Is the [device] extra installed?")
        return 1

    try:
        link = SerialByteLink(args.port, args.baud)
    except Exception as exc:
        print(f"error: cannot open serial port {args.port!r}: {exc}")
        return 1

    try:
        wake_console_handoff(link)  # console `sync` command -> framed mode
        client = DeviceClient(link)
        host_time = datetime.now().astimezone().isoformat(timespec="seconds")
        info = client.hello(host_time)
        check_schema_compatible(info)
        print(f"device    fw {info.fw_build}  schema v{info.schema_version}"
              f"  proto v{info.proto_version}")
        print(f"time      set to {host_time}")
        if not info.storage_ok:
            print("error: device reports storage FAILED (card missing/full/corrupt).")
            print("       The box still captures and clicks; fix the card, then re-sync.")
            client.bye()
            return 1

        report = sync_sessions(client, Path(args.out))
        client.bye()

        for name in report.fetched:
            print(f"fetched   {name}")
        for name in report.skipped:
            print(f"skipped   {name} (already synced, checksum verified)")
        for name in report.conflicts:
            print(f"CONFLICT  {name} — local file differs from device copy; NOT overwritten")
        for warning in report.warnings:
            print(f"warning   {warning}")
        print(f"done: {len(report.fetched)} fetched, {len(report.skipped)} skipped,"
              f" {len(report.conflicts)} conflict(s) -> {args.out}")
        return 0 if report.ok else 1
    except DeviceStorageError as exc:
        print(f"error: {exc}")
        return 1
    except DeviceError as exc:
        print(f"error: {exc}")
        print("hint: is the box on this port, and did the console print '[sync] framed mode'?")
        return 1
    finally:
        try:
            link.close()
        except Exception:
            pass


def cmd_rm(args) -> int:
    """Delete one closed session file off the capture box's SD card (capture
    spec §13.1): a narrow, explicit, host-requested act — the device never
    deletes anything on its own. Refuses the actively-open session."""
    try:
        from edrum.io.devlink import (
            DeviceClient,
            DeviceError,
            DeviceStorageError,
            SerialByteLink,
            wake_console_handoff,
        )
    except ImportError as exc:
        print(f"error: device link unavailable ({exc}). Is the [device] extra installed?")
        return 1

    try:
        link = SerialByteLink(args.port, args.baud)
    except Exception as exc:
        print(f"error: cannot open serial port {args.port!r}: {exc}")
        return 1

    try:
        wake_console_handoff(link)
        client = DeviceClient(link)
        info = client.hello()
        if not info.storage_ok:
            print("error: device reports storage FAILED (card missing/full/corrupt).")
            client.bye()
            return 1
        client.delete(args.name)
        client.bye()
        print(f"deleted   {args.name}")
        return 0
    except DeviceStorageError as exc:
        print(f"error: {exc}")
        return 1
    except DeviceError as exc:
        print(f"error: {exc}")
        print("hint: is the box on this port, and did the console print '[sync] framed mode'?")
        return 1
    finally:
        try:
            link.close()
        except Exception:
            pass


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

    p_an = sub.add_parser("analyze", help="Layer A analysis of a session file")
    p_an.add_argument("file")
    p_an.add_argument("--json", action="store_true", help="emit the full report as JSON")
    p_an.add_argument(
        "--kit", default=None,
        help="override the kit profile id (re-pointable pointer, spec §4.2)",
    )
    p_an.add_argument(
        "--profiles-dir", default=str(default_profiles_dir()), help="profile registry directory"
    )
    p_an.add_argument(
        "--metrics", default=None,
        help="comma-separated analyzer ids to run (default: all registered)",
    )
    p_an.set_defaults(func=cmd_analyze)

    p_sync = sub.add_parser("sync", help="pull session files off the capture box")
    p_sync.add_argument("--port", required=True, help="serial port (e.g. COM5, /dev/ttyUSB0)")
    p_sync.add_argument("--baud", type=int, default=115200, help="baud rate (default: 115200)")
    p_sync.add_argument("--out", default="sessions", help="output directory (default: sessions)")
    p_sync.set_defaults(func=cmd_sync)

    p_rm = sub.add_parser("rm", help="delete one closed session off the capture box's SD card")
    p_rm.add_argument("--port", required=True, help="serial port (e.g. COM5, /dev/ttyUSB0)")
    p_rm.add_argument("--baud", type=int, default=115200, help="baud rate (default: 115200)")
    p_rm.add_argument("name", help="exact session filename on the device (see `edrum sync` output)")
    p_rm.set_defaults(func=cmd_rm)

    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
