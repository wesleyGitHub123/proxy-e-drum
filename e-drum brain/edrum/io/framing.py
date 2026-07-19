"""Wire framing for the device link — the brain-side mirror of the firmware's
``edrum/frame.h`` (capture spec §13): COBS byte stuffing + CRC-16/CCITT-FALSE.

Both sides implement the identical discipline; the shared test vectors
(``tests/test_devlink.py`` ↔ firmware ``test/native/test_frame``) are the
cross-language contract — a change that breaks them breaks the wire.

Wire form of one frame::

    0x00  COBS( payload || crc16_le(payload) )  0x00

COBS output contains no zero bytes, so ``0x00`` is an unambiguous delimiter
and the receiver resynchronizes after arbitrary garbage (e.g. console text
sharing the serial line before the modal handoff completes).
"""

from __future__ import annotations

FRAME_PAYLOAD_MAX = 520


class FramingError(Exception):
    """Malformed COBS segment or CRC mismatch."""


def crc16(data: bytes, seed: int = 0xFFFF) -> int:
    """CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflect/xor-out).

    ``crc16(b"123456789") == 0x29B1``. Chainable: pass a previous result as
    ``seed`` to continue over split input — the sync protocol checksums large
    files in bounded chunks this way.
    """
    crc = seed
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def cobs_encode(data: bytes) -> bytes:
    """COBS encode (streaming variant, matching the firmware: always emits a
    final code byte, so input ending exactly on a 254-byte run gains a
    trailing ``0x01`` group — decoders accept both encodings)."""
    out = bytearray([0])  # placeholder for the first code byte
    code_i = 0
    code = 1
    for b in data:
        if b == 0:
            out[code_i] = code
            code_i = len(out)
            out.append(0)
            code = 1
        else:
            out.append(b)
            code += 1
            if code == 0xFF:
                out[code_i] = code
                code_i = len(out)
                out.append(0)
                code = 1
    out[code_i] = code
    return bytes(out)


def cobs_decode(data: bytes) -> bytes:
    """COBS decode; raises :class:`FramingError` on malformed input."""
    out = bytearray()
    i = 0
    n = len(data)
    while i < n:
        code = data[i]
        i += 1
        if code == 0:
            raise FramingError("delimiter byte inside COBS data")
        if i + code - 1 > n:
            raise FramingError("truncated COBS group")
        group = data[i : i + code - 1]
        if 0 in group:
            raise FramingError("delimiter byte inside COBS group")
        out.extend(group)
        i += code - 1
        if code != 0xFF and i < n:
            out.append(0)
    return bytes(out)


def frame_encode(payload: bytes) -> bytes:
    """payload -> full wire frame (leading 0x00, COBS(payload||crc), 0x00)."""
    if not payload or len(payload) > FRAME_PAYLOAD_MAX:
        raise FramingError(f"payload length {len(payload)} out of range")
    crc = crc16(payload)
    staged = payload + bytes([crc & 0xFF, crc >> 8])
    return b"\x00" + cobs_encode(staged) + b"\x00"


class FrameAccumulator:
    """Byte-stream reassembly: feed chunks as they arrive, get back the
    CRC-verified payloads. Garbage between delimiters (console text, bad
    CRC) is dropped and counted; the stream resyncs on the next ``0x00``."""

    def __init__(self) -> None:
        self._buf = bytearray()
        self.bad_frames = 0

    def feed(self, data: bytes) -> list[bytes]:
        payloads: list[bytes] = []
        self._buf.extend(data)
        while (i := self._buf.find(0)) != -1:
            segment = bytes(self._buf[:i])
            del self._buf[: i + 1]
            if not segment:
                continue  # leading/duplicate delimiter
            try:
                decoded = cobs_decode(segment)
                if len(decoded) < 3:  # >= 1 payload byte + 2 crc bytes
                    raise FramingError("segment too short")
                payload, crc_lo, crc_hi = decoded[:-2], decoded[-2], decoded[-1]
                if crc16(payload) != (crc_lo | (crc_hi << 8)):
                    raise FramingError("crc mismatch")
            except FramingError:
                self.bad_frames += 1
                continue
            payloads.append(payload)
        return payloads
