"""PC side of the UART protocol (see firmware/protocol.h for the definition)."""
import binascii
import struct
import time
from dataclasses import dataclass, field

SYNC_REQ = b"\xAA\x55"
SYNC_RSP = b"\x55\xAA"

CMD_CNN, CMD_SNN, CMD_BOTH, CMD_ECHO, CMD_INFO = 0x01, 0x02, 0x03, 0x10, 0x11
STATUS = {0: "OK", 1: "BAD_CRC", 2: "BAD_LEN", 3: "BAD_CMD", 4: "NO_NET", 5: "TIMEOUT"}
NET_NAME = {1: "CNN", 2: "SNN"}
RESULT_BYTES = 26


def crc16(data: bytes) -> int:
    return binascii.crc_hqx(data, 0xFFFF)


def build_request(cmd: int, payload: bytes = b"") -> bytes:
    body = struct.pack("<BH", cmd, len(payload)) + payload
    return SYNC_REQ + body + struct.pack("<H", crc16(body))


def build_response(cmd: int, status: int, payload: bytes = b"") -> bytes:
    body = struct.pack("<BBH", cmd, status, len(payload)) + payload
    return SYNC_RSP + body + struct.pack("<H", crc16(body))


@dataclass
class NetResult:
    net: str
    pred: int
    t_pre_us: int
    t_ip_us: int
    logits: tuple

    @property
    def scores(self):
        """Logits in natural units (CNN Q8.8, SNN membrane scale 128)."""
        return tuple(l / (256.0 if self.net == "CNN" else 128.0) for l in self.logits)


@dataclass
class Reply:
    cmd: int
    status: int
    payload: bytes
    rtt_s: float = 0.0          # PC wall-clock: first byte sent -> last byte received
    t_rx_us: int = 0
    results: list = field(default_factory=list)

    @property
    def ok(self):
        return self.status == 0


def parse_inference(reply: Reply) -> Reply:
    p = reply.payload
    if not reply.ok or len(p) < 4:
        return reply
    reply.t_rx_us = struct.unpack_from("<I", p, 0)[0]
    for off in range(4, len(p), RESULT_BYTES):
        net, pred, t_pre, t_ip, *lg = struct.unpack_from("<BBII4i", p, off)
        reply.results.append(NetResult(NET_NAME.get(net, str(net)), pred, t_pre, t_ip, tuple(lg)))
    return reply


class Link:
    """Framed request/response over a pyserial-like port (read/write/reset_input_buffer)."""

    def __init__(self, port, timeout=3.0):
        self.port = port
        self.timeout = timeout

    def _read_exact(self, n, deadline):
        buf = b""
        while len(buf) < n:
            if time.perf_counter() > deadline:
                raise TimeoutError(f"timeout, got {len(buf)}/{n} bytes")
            buf += self.port.read(n - len(buf))
        return buf

    def transact(self, cmd: int, payload: bytes = b"") -> Reply:
        req = build_request(cmd, payload)
        self.port.reset_input_buffer()
        t0 = time.perf_counter()
        self.port.write(req)
        deadline = t0 + self.timeout
        # resync on SYNC_RSP (skips any boot banner text)
        window = b""
        while window != SYNC_RSP:
            window = (window + self._read_exact(1, deadline))[-2:]
        hdr = self._read_exact(4, deadline)
        cmd_r, status, n = struct.unpack("<BBH", hdr)
        body = self._read_exact(n + 2, deadline)
        rtt = time.perf_counter() - t0
        payload, crc = body[:n], struct.unpack("<H", body[n:])[0]
        if crc != crc16(hdr + payload):
            raise IOError("response CRC mismatch")
        if cmd_r != cmd:
            raise IOError(f"response for cmd {cmd_r:#x}, expected {cmd:#x}")
        return Reply(cmd_r, status, payload, rtt)

    def infer(self, img_u8, cmd=CMD_BOTH) -> Reply:
        return parse_inference(self.transact(cmd, bytes(img_u8.reshape(-1))))

    def info(self):
        r = self.transact(CMD_INFO)
        have_cnn, have_snn, baud, timer_hz, ver = struct.unpack("<BBIIH", r.payload)
        return dict(have_cnn=bool(have_cnn), have_snn=bool(have_snn), baud=baud,
                    timer_hz=timer_hz, version=hex(ver))


def uart_time_s(n_bytes: int, baud: int) -> float:
    """Wire time for n bytes at 8N1 (10 bits per byte)."""
    return n_bytes * 10 / baud
