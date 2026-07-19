"""Device link — the brain side of the session-archive sync protocol
(capture spec §13). The device is the writer-of-record; this client
replicates its archive **byte-exactly** into the local ``sessions/``
directory. The brain must never be able to tell a synced file from a
card-reader copy: same names, same bytes — the engine and ``FileSource``
are untouched by this module's existence.

Layering mirrors the firmware:
    * :mod:`edrum.io.framing` — COBS + CRC-16 wire discipline (frame.h twin)
    * this module — protocol client over an abstract :class:`ByteLink`
    * :class:`SerialByteLink` — transport #1 (pyserial, ``[device]`` extra);
      a TCP/BLE link later is a drop-in second implementation

Protocol requests are all idempotent reads (FETCH/CRC are pure; HELLO/BYE
are repeatable), so timeout handling is a plain bounded retry.

Fail-soft (capture decision 5): a device with dead storage answers
``ERR StorageFailed`` on the wire; this surfaces as the distinct
:class:`DeviceStorageError` rather than a generic failure.
"""

from __future__ import annotations

import time
from collections import deque
from dataclasses import dataclass, field
from enum import IntEnum
from pathlib import Path
from typing import Protocol

from edrum.engine.records import SCHEMA_VERSION
from edrum.io.framing import FrameAccumulator, crc16, frame_encode

PROTO_VERSION = 1
CHANNEL_SYNC = 0

FETCH_CHUNK = 512  # mirrors firmware sync::kFetchMax
CRC_CHUNK = 4096   # mirrors firmware sync::kCrcChunkMax


class Type(IntEnum):
    HELLO = 0x01
    LIST = 0x02
    FETCH = 0x03
    CRC = 0x04
    BYE = 0x05
    DELETE = 0x06
    HELLO_OK = 0x81
    ITEM = 0x82
    LIST_END = 0x83
    DATA = 0x84
    CRC_OK = 0x85
    BYE_OK = 0x86
    DELETE_OK = 0x87
    ERR = 0xFF


class ErrCode(IntEnum):
    BAD_REQUEST = 1
    UNKNOWN_TYPE = 2
    UNKNOWN_NAME = 3
    BAD_RANGE = 4
    STORAGE_FAILED = 5
    UNSUPPORTED_VERSION = 6
    SESSION_OPEN = 7  # DELETE refuses the actively-written session


class DeviceError(Exception):
    """Link/protocol failure talking to the capture device."""


class DeviceStorageError(DeviceError):
    """The device reported its storage is down (explicit fail-soft state):
    the box still captures and clicks, but nothing can be listed/fetched
    until the card is fixed."""


class ProtocolError(DeviceError):
    """Version refusal or a malformed/unexpected response."""


class ByteLink(Protocol):
    """What the client asks of a transport: a plain byte pipe."""

    def read(self, timeout: float) -> bytes: ...

    def write(self, data: bytes) -> None: ...


class SerialByteLink:
    """Transport #1: the device's UART/USB console line via pyserial
    (``[device]`` extra). The console is handed over modally — see
    :func:`wake_console_handoff`."""

    def __init__(self, port: str, baud: int = 115200) -> None:
        import serial  # local import: engine/file tooling must not need pyserial

        self._ser = serial.Serial(port, baud, timeout=0)

    def read(self, timeout: float) -> bytes:
        deadline = time.monotonic() + timeout
        while True:
            pending = self._ser.in_waiting
            if pending:
                return self._ser.read(pending)
            if time.monotonic() >= deadline:
                return b""
            time.sleep(0.01)

    def write(self, data: bytes) -> None:
        self._ser.write(data)

    def close(self) -> None:
        self._ser.close()


def wake_console_handoff(link: ByteLink) -> None:
    """Serial-transport-specific: type the ``sync`` console command so the
    device suspends its interactive console and hands the line to the framed
    protocol (capture decision 1). Any text the console prints back is
    garbage to the frame accumulator, which resynchronizes on the first
    frame delimiter — no parsing of console output is ever needed."""
    link.write(b"\nsync\n")
    time.sleep(0.3)  # let the console switch modes before the first frame


@dataclass(frozen=True)
class DeviceInfo:
    proto_version: int
    schema_version: int
    storage_ok: bool
    fw_build: str


@dataclass(frozen=True)
class SessionEntry:
    name: str
    size: int


@dataclass
class SyncReport:
    fetched: list[str] = field(default_factory=list)
    skipped: list[str] = field(default_factory=list)
    conflicts: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return not self.conflicts


class DeviceClient:
    """Request/response client. One outstanding request at a time (the
    protocol is lockstep by design — inherent flow control)."""

    def __init__(self, link: ByteLink, *, timeout: float = 2.0, retries: int = 2) -> None:
        self._link = link
        self._timeout = timeout
        self._retries = retries
        self._acc = FrameAccumulator()
        self._pending: deque[bytes] = deque()

    # -- low-level ----------------------------------------------------------

    def _next_frame(self, deadline: float) -> bytes:
        """One sync-channel payload; frames arriving in the same read burst
        (LIST responses) queue in order and are consumed one at a time."""
        while not self._pending:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise DeviceError("timeout waiting for device response")
            for payload in self._acc.feed(self._link.read(remaining)):
                if payload and payload[0] == CHANNEL_SYNC:
                    self._pending.append(payload)
        return self._pending.popleft()

    @staticmethod
    def _check_err(payload: bytes) -> None:
        if len(payload) >= 3 and payload[1] == Type.ERR:
            code = payload[2]
            if code == ErrCode.STORAGE_FAILED:
                raise DeviceStorageError(
                    "device storage failed (card missing/full/corrupt) — "
                    "the box keeps capturing and clicking, but sync needs the card fixed"
                )
            if code == ErrCode.UNSUPPORTED_VERSION:
                raise ProtocolError(
                    f"device refused protocol version {PROTO_VERSION} "
                    "(firmware newer/older than this edrum install)"
                )
            raise ProtocolError(f"device error: {ErrCode(code).name.lower()}"
                                if code in ErrCode._value2member_map_
                                else f"device error code {code}")

    def _request(self, request: bytes, expect: Type) -> bytes:
        """Send one request; return the single expected response payload.
        Lockstep protocol: anything still pending is a stale leftover of an
        earlier retransmission and is dropped before sending."""
        last: Exception | None = None
        for _ in range(self._retries + 1):
            self._pending.clear()
            self._link.write(frame_encode(request))
            try:
                payload = self._next_frame(time.monotonic() + self._timeout)
            except DeviceError as exc:  # timeout: all requests idempotent -> retry
                last = exc
                continue
            self._check_err(payload)
            if payload[1] != expect:
                raise ProtocolError(f"expected {expect.name}, got 0x{payload[1]:02x}")
            return payload
        raise last if last is not None else DeviceError("request failed")

    # -- protocol operations --------------------------------------------------

    def hello(self, host_time_iso: str | None = None) -> DeviceInfo:
        iso = (host_time_iso or "").encode()
        payload = self._request(
            bytes([CHANNEL_SYNC, Type.HELLO, PROTO_VERSION, len(iso)]) + iso, Type.HELLO_OK
        )
        if len(payload) < 6:
            raise ProtocolError("short HELLO_OK")
        fw_len = payload[5]
        return DeviceInfo(
            proto_version=payload[2],
            schema_version=payload[3],
            storage_ok=bool(payload[4]),
            fw_build=payload[6 : 6 + fw_len].decode(errors="replace"),
        )

    def list_sessions(self) -> list[SessionEntry]:
        self._pending.clear()
        self._link.write(frame_encode(bytes([CHANNEL_SYNC, Type.LIST])))
        entries: list[SessionEntry] = []
        deadline = time.monotonic() + self._timeout
        while True:
            payload = self._next_frame(deadline)
            self._check_err(payload)
            if payload[1] == Type.LIST_END:
                count = payload[2] | (payload[3] << 8)
                if count != len(entries):
                    raise ProtocolError(f"LIST_END count {count} != {len(entries)} items")
                return entries
            if payload[1] != Type.ITEM:
                raise ProtocolError(f"expected ITEM/LIST_END, got 0x{payload[1]:02x}")
            nlen = payload[2]
            name = payload[3 : 3 + nlen].decode()
            size = int.from_bytes(payload[3 + nlen : 3 + nlen + 4], "little")
            entries.append(SessionEntry(name=name, size=size))
            deadline = time.monotonic() + self._timeout  # progress resets it

    @staticmethod
    def _named(t: Type, name: str, offset: int, length: int, seed: int | None = None) -> bytes:
        nb = name.encode()
        body = bytes([len(nb)]) + nb + offset.to_bytes(4, "little") + length.to_bytes(2, "little")
        if seed is not None:
            body += seed.to_bytes(2, "little")
        return bytes([CHANNEL_SYNC, t]) + body

    def fetch(self, name: str, offset: int, length: int) -> bytes:
        payload = self._request(self._named(Type.FETCH, name, offset, length), Type.DATA)
        return payload[2:]

    def crc(self, name: str, offset: int, length: int, seed: int = 0xFFFF) -> int:
        payload = self._request(self._named(Type.CRC, name, offset, length, seed), Type.CRC_OK)
        return payload[2] | (payload[3] << 8)

    def file_crc(self, name: str, length: int) -> int:
        """CRC of the file's first `length` bytes via bounded, chained chunks
        (the device never holds the SD longer than one CRC_CHUNK per request
        — capture always wins). `length` = file size gives the whole file;
        a shorter length verifies a resume prefix."""
        crc = 0xFFFF
        offset = 0
        while offset < length:
            span = min(CRC_CHUNK, length - offset)
            crc = self.crc(name, offset, span, crc)
            offset += span
        return crc

    def delete(self, name: str) -> None:
        """Remove one closed session file from the device's archive (capture
        spec §13.1: a narrow, explicit, host-requested act — the device
        never deletes anything on its own). Raises DeviceError (via
        ProtocolError's generic path) with code ``session_open`` if `name`
        is the file currently being written."""
        nb = name.encode()
        self._request(bytes([CHANNEL_SYNC, Type.DELETE, len(nb)]) + nb, Type.DELETE_OK)

    def bye(self) -> None:
        self._request(bytes([CHANNEL_SYNC, Type.BYE]), Type.BYE_OK)


# -- the high-level sync ------------------------------------------------------

def sync_sessions(client: DeviceClient, out_dir: Path) -> SyncReport:
    """Replicate the device archive into ``out_dir`` (idempotent, resumable).

    Corpus protection (brain spec §3 — files are never rewritten): an
    existing local file is NEVER overwritten. Identical (size + CRC) ->
    skipped; different -> reported as a conflict and left untouched.
    Partial downloads land in ``<name>.part`` and are renamed only after
    size, CRC, and a parse check all pass.
    """
    report = SyncReport()
    out_dir.mkdir(parents=True, exist_ok=True)

    for entry in client.list_sessions():
        final = out_dir / entry.name
        if final.exists():
            local = final.read_bytes()
            if len(local) == entry.size and crc16(local) == client.file_crc(entry.name, entry.size):
                report.skipped.append(entry.name)
            else:
                report.conflicts.append(entry.name)
            continue

        part = out_dir / (entry.name + ".part")
        offset = 0
        if part.exists():
            existing = part.read_bytes()
            if 0 < len(existing) <= entry.size and client.file_crc(
                entry.name, len(existing)
            ) == crc16(existing):
                offset = len(existing)  # verified prefix: resume
            else:
                part.unlink()  # stale/corrupt partial: start over

        with part.open("ab") as f:
            while offset < entry.size:
                data = client.fetch(entry.name, offset, min(FETCH_CHUNK, entry.size - offset))
                if not data:
                    raise DeviceError(
                        f"{entry.name}: device returned EOF at {offset}/{entry.size}"
                    )
                f.write(data)
                offset += len(data)

        blob = part.read_bytes()
        if len(blob) != entry.size:
            raise DeviceError(f"{entry.name}: size mismatch after fetch")
        if crc16(blob) != client.file_crc(entry.name, entry.size):
            part.unlink()
            raise DeviceError(f"{entry.name}: checksum mismatch — transfer corrupted, removed")

        # sanity gate: the landed bytes are a readable session log (the
        # byte-identity replay gate stays the definitive check: edrum replay)
        from edrum.io.logfile import read_log

        lr = read_log(part)
        if lr.recovery.truncated:
            report.warnings.append(
                f"{entry.name}: crash-recovered on device "
                f"({lr.recovery.dropped_bytes} torn byte(s) will drop on read)"
            )

        if final.exists():  # extremely defensive: appeared mid-transfer
            report.conflicts.append(entry.name)
            continue
        part.rename(final)
        report.fetched.append(entry.name)

    return report


def check_schema_compatible(info: DeviceInfo) -> None:
    """Mirror of the corpus reader policy (brain spec §3): refuse files from
    a newer-major producer up front, at handshake time."""
    if info.schema_version > SCHEMA_VERSION:
        raise ProtocolError(
            f"device writes schema v{info.schema_version}; this edrum install reads "
            f"<= v{SCHEMA_VERSION} — update the brain before syncing"
        )
