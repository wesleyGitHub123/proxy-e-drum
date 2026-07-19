"""Device link (capture spec §13): framing vectors shared with the firmware
(test/native/test_frame — the cross-language wire contract), the protocol
client, and the high-level sync against a scripted fake device.

``FakeDeviceLink`` implements the DEVICE side of the protocol in Python,
mirroring the firmware's SyncService semantics — it is the executable spec
the two implementations meet at, the same role FakeSource plays for capture.
"""

from __future__ import annotations

from pathlib import Path

import pytest

from edrum.engine.records import SCHEMA_VERSION
from edrum.io.devlink import (
    CHANNEL_SYNC,
    CRC_CHUNK,
    FETCH_CHUNK,
    PROTO_VERSION,
    DeviceClient,
    DeviceError,
    DeviceStorageError,
    ErrCode,
    ProtocolError,
    Type,
    check_schema_compatible,
    sync_sessions,
)
from edrum.io.framing import (
    FrameAccumulator,
    FramingError,
    cobs_decode,
    cobs_encode,
    crc16,
    frame_encode,
)

FIXTURES = Path(__file__).parent / "fixtures"


# ---------------------------------------------------------------------------
# framing — vectors shared byte-for-byte with firmware test_frame
# ---------------------------------------------------------------------------

def test_crc16_check_values():
    assert crc16(b"123456789") == 0x29B1  # CRC-16/CCITT-FALSE canonical
    assert crc16(b"") == 0xFFFF
    # chaining: crc(a||b) == crc(b, seed=crc(a)) — the bounded-chunk file
    # checksum depends on this
    assert crc16(b"6789", seed=crc16(b"12345")) == crc16(b"123456789")


def test_cobs_vectors():
    assert cobs_encode(b"") == bytes([0x01])
    assert cobs_encode(b"\x00") == bytes([0x01, 0x01])
    assert cobs_encode(bytes([0x11, 0x22, 0x00, 0x33])) == bytes([0x03, 0x11, 0x22, 0x02, 0x33])
    # 254-byte run: streaming variant emits the trailing 0x01 group
    run = bytes([0x01]) * 254
    enc = cobs_encode(run)
    assert enc[0] == 0xFF and enc[-1] == 0x01 and len(enc) == 256
    assert cobs_decode(enc) == run


def test_cobs_roundtrip_and_rejects():
    for payload in (b"\x00\x00\x00", bytes(range(1, 255)), bytes(range(256)) * 2):
        enc = cobs_encode(payload)
        assert 0 not in enc
        assert cobs_decode(enc) == payload
    with pytest.raises(FramingError):
        cobs_decode(bytes([0x03, 0x11, 0x00]))  # embedded zero
    with pytest.raises(FramingError):
        cobs_decode(bytes([0x03, 0x11]))  # truncated group


def test_frame_roundtrip_resync_and_crc_reject():
    payload = bytes([CHANNEL_SYNC, Type.HELLO, PROTO_VERSION, 0])
    wire = frame_encode(payload)
    assert wire[0] == 0 and wire[-1] == 0

    acc = FrameAccumulator()
    # console text garbage, then two clean frames, split across feeds
    stream = b"[click] 120 bpm /4\r\n" + wire + wire
    got = acc.feed(stream[:7]) + acc.feed(stream[7:])
    assert got == [payload, payload]
    assert acc.bad_frames == 1  # the text segment

    corrupt = bytearray(wire)
    corrupt[2] ^= 0x40
    assert acc.feed(bytes(corrupt)) == []
    assert acc.bad_frames == 2


# ---------------------------------------------------------------------------
# the fake device — executable spec of the firmware SyncService
# ---------------------------------------------------------------------------

class FakeDeviceLink:
    """ByteLink whose far end is a scripted capture box."""

    def __init__(
        self,
        files: dict[str, bytes],
        *,
        storage_ok: bool = True,
        open_name: str | None = None,
        proto_version: int = PROTO_VERSION,
        schema_version: int = SCHEMA_VERSION,
        leading_garbage: bytes = b"",
    ) -> None:
        self.files = files
        self.storage_ok = storage_ok
        self.open_name = open_name
        self.proto_version = proto_version
        self.schema_version = schema_version
        self.host_time: str | None = None
        self.saw_bye = False
        self.fetch_offsets: list[int] = []
        self._rx = FrameAccumulator()
        self._out = bytearray(leading_garbage)

    # -- ByteLink -------------------------------------------------------------

    def write(self, data: bytes) -> None:
        for payload in self._rx.feed(data):
            self._handle(payload)

    def read(self, timeout: float) -> bytes:
        out = bytes(self._out)
        self._out.clear()
        return out

    def close(self) -> None:
        pass

    # -- device side ----------------------------------------------------------

    def _emit(self, payload: bytes) -> None:
        self._out += frame_encode(payload)

    def _err(self, code: ErrCode) -> None:
        self._emit(bytes([CHANNEL_SYNC, Type.ERR, code]))

    def _handle(self, p: bytes) -> None:
        if len(p) < 2 or p[0] != CHANNEL_SYNC:
            return
        t, body = p[1], p[2:]
        if t == Type.HELLO:
            if body[0] != self.proto_version:
                self._err(ErrCode.UNSUPPORTED_VERSION)
                return
            tlen = body[1]
            if tlen:
                self.host_time = body[2 : 2 + tlen].decode()
            fw = b"fake-device"
            self._emit(
                bytes([CHANNEL_SYNC, Type.HELLO_OK, self.proto_version, self.schema_version,
                       1 if self.storage_ok else 0, len(fw)]) + fw
            )
        elif t == Type.LIST:
            if not self.storage_ok:
                self._err(ErrCode.STORAGE_FAILED)
                return
            count = 0
            for name, blob in self.files.items():
                if name == self.open_name:
                    continue
                nb = name.encode()
                self._emit(bytes([CHANNEL_SYNC, Type.ITEM, len(nb)]) + nb
                           + len(blob).to_bytes(4, "little"))
                count += 1
            self._emit(bytes([CHANNEL_SYNC, Type.LIST_END]) + count.to_bytes(2, "little"))
        elif t in (Type.FETCH, Type.CRC):
            if not self.storage_ok:
                self._err(ErrCode.STORAGE_FAILED)
                return
            nlen = body[0]
            name = body[1 : 1 + nlen].decode()
            rest = body[1 + nlen :]
            offset = int.from_bytes(rest[0:4], "little")
            length = int.from_bytes(rest[4:6], "little")
            if name not in self.files:
                self._err(ErrCode.UNKNOWN_NAME)
                return
            blob = self.files[name]
            if t == Type.FETCH:
                length = min(length, FETCH_CHUNK)
                if offset > len(blob):
                    self._err(ErrCode.BAD_RANGE)
                    return
                self.fetch_offsets.append(offset)
                self._emit(bytes([CHANNEL_SYNC, Type.DATA]) + blob[offset : offset + length])
            else:
                length = min(length, CRC_CHUNK)
                seed = int.from_bytes(rest[6:8], "little")
                if offset + length > len(blob):
                    self._err(ErrCode.BAD_RANGE)
                    return
                crc = crc16(blob[offset : offset + length], seed)
                self._emit(bytes([CHANNEL_SYNC, Type.CRC_OK]) + crc.to_bytes(2, "little"))
        elif t == Type.DELETE:
            if not self.storage_ok:
                self._err(ErrCode.STORAGE_FAILED)
                return
            nlen = body[0]
            name = body[1 : 1 + nlen].decode()
            if name == self.open_name:
                self._err(ErrCode.SESSION_OPEN)
                return
            if name not in self.files:
                self._err(ErrCode.UNKNOWN_NAME)
                return
            del self.files[name]
            self._emit(bytes([CHANNEL_SYNC, Type.DELETE_OK]))
        elif t == Type.BYE:
            self.saw_bye = True
            self._emit(bytes([CHANNEL_SYNC, Type.BYE_OK]))
        else:
            self._err(ErrCode.UNKNOWN_TYPE)


def fixture_files() -> dict[str, bytes]:
    return {
        "20260706T120000_11111111.jsonl": (FIXTURES / "minimal.jsonl").read_bytes(),
        "20260707T090000_22222222.jsonl": (FIXTURES / "declarations.jsonl").read_bytes(),
    }


def make_client(link: FakeDeviceLink) -> DeviceClient:
    return DeviceClient(link, timeout=0.5, retries=0)


# ---------------------------------------------------------------------------
# protocol operations
# ---------------------------------------------------------------------------

def test_hello_list_fetch_crc_roundtrip():
    link = FakeDeviceLink(fixture_files())
    client = make_client(link)

    info = client.hello("2026-07-12T10:30:00+08:00")
    assert info.proto_version == PROTO_VERSION
    assert info.schema_version == SCHEMA_VERSION
    assert info.storage_ok and info.fw_build == "fake-device"
    assert link.host_time == "2026-07-12T10:30:00+08:00"  # settime rode along

    entries = client.list_sessions()
    assert [e.name for e in entries] == list(fixture_files())
    blob = link.files[entries[0].name]
    assert entries[0].size == len(blob)

    assert client.fetch(entries[0].name, 0, 64) == blob[:64]
    assert client.file_crc(entries[0].name, len(blob)) == crc16(blob)

    client.bye()
    assert link.saw_bye


def test_hello_survives_console_garbage_before_first_frame():
    # the console prints its handoff message before framed mode: the client
    # must resync past it (frame delimiters make text unambiguous garbage)
    link = FakeDeviceLink(fixture_files(),
                          leading_garbage=b"[sync] framed mode: console suspended\r\n")
    info = make_client(link).hello(None)
    assert info.storage_ok


def test_version_and_schema_refusals():
    # device on a newer protocol: explicit refusal, never guessing
    link = FakeDeviceLink(fixture_files(), proto_version=PROTO_VERSION + 1)
    with pytest.raises(ProtocolError):
        make_client(link).hello(None)

    # device writing a newer schema major: refuse at handshake (reader policy)
    link = FakeDeviceLink(fixture_files(), schema_version=SCHEMA_VERSION + 1)
    info = make_client(link).hello(None)
    with pytest.raises(ProtocolError):
        check_schema_compatible(info)


def test_delete_removes_closed_session():
    files = fixture_files()
    victim, keep = list(files)
    link = FakeDeviceLink(files)
    client = make_client(link)

    client.delete(victim)
    assert victim not in link.files
    assert keep in link.files

    entries = client.list_sessions()
    assert [e.name for e in entries] == [keep]


def test_delete_refuses_the_open_session():
    files = fixture_files()
    open_name = next(iter(files))
    link = FakeDeviceLink(files, open_name=open_name)
    client = make_client(link)

    with pytest.raises(ProtocolError):
        client.delete(open_name)
    assert open_name in link.files  # refused, not removed


def test_delete_unknown_name_is_explicit():
    link = FakeDeviceLink(fixture_files())
    client = make_client(link)

    with pytest.raises(ProtocolError):
        client.delete("nope.jsonl")


def test_storage_failed_surfaces_distinctly():
    link = FakeDeviceLink(fixture_files(), storage_ok=False)
    client = make_client(link)
    info = client.hello(None)
    assert not info.storage_ok  # visible at handshake
    with pytest.raises(DeviceStorageError):
        client.list_sessions()  # and explicit on the wire (decision 5)


# ---------------------------------------------------------------------------
# sync_sessions — the end-to-end replication logic
# ---------------------------------------------------------------------------

def test_sync_lands_byte_identical_files_and_is_idempotent(tmp_path):
    files = fixture_files()
    link = FakeDeviceLink(files)
    client = make_client(link)

    report = sync_sessions(client, tmp_path)
    assert sorted(report.fetched) == sorted(files)
    assert not report.conflicts and not report.skipped
    for name, blob in files.items():
        assert (tmp_path / name).read_bytes() == blob  # the sneakernet test
        assert not (tmp_path / (name + ".part")).exists()

    # second run: everything checksum-verified and skipped, nothing rewritten
    report2 = sync_sessions(client, tmp_path)
    assert sorted(report2.skipped) == sorted(files)
    assert not report2.fetched


def test_sync_excludes_open_session():
    files = fixture_files()
    open_name = next(iter(files))
    link = FakeDeviceLink(files, open_name=open_name)
    entries = make_client(link).list_sessions()
    assert open_name not in [e.name for e in entries]


def test_sync_never_overwrites_a_differing_local_file(tmp_path):
    files = fixture_files()
    name = next(iter(files))
    local = tmp_path / name
    local.write_bytes(b'{"type":"meta","schema_version":2}\n')  # different content

    report = sync_sessions(make_client(FakeDeviceLink(files)), tmp_path)
    assert name in report.conflicts
    assert local.read_bytes().startswith(b'{"type":"meta"')  # untouched
    assert not report.ok


def test_sync_resumes_verified_partial(tmp_path):
    files = fixture_files()
    name = next(iter(files))
    blob = files[name]
    prefix_len = 100
    (tmp_path / (name + ".part")).write_bytes(blob[:prefix_len])

    link = FakeDeviceLink({name: blob})
    report = sync_sessions(make_client(link), tmp_path)
    assert report.fetched == [name]
    assert (tmp_path / name).read_bytes() == blob
    assert link.fetch_offsets[0] == prefix_len  # resumed, not restarted


def test_sync_discards_corrupt_partial_and_refetches(tmp_path):
    files = fixture_files()
    name = next(iter(files))
    blob = files[name]
    (tmp_path / (name + ".part")).write_bytes(b"garbage that matches nothing")

    link = FakeDeviceLink({name: blob})
    report = sync_sessions(make_client(link), tmp_path)
    assert report.fetched == [name]
    assert (tmp_path / name).read_bytes() == blob
    assert link.fetch_offsets[0] == 0  # started over


def test_timeout_is_a_device_error():
    class DeadLink:
        def read(self, timeout: float) -> bytes:
            return b""

        def write(self, data: bytes) -> None:
            pass

    client = DeviceClient(DeadLink(), timeout=0.05, retries=1)
    with pytest.raises(DeviceError):
        client.hello(None)
