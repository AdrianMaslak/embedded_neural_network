"""
Software stand-in for the PYNQ-Z2 board: a fake serial port that speaks the
same protocol and answers with the software reference models
(Keras CNN, bit-exact integer SNN). Lets the PC app be tested without hardware:

    python pc/app.py --sim eval
"""
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import IMG_H, IMG_W, ROOT, cnn_normalize, snn_features_int16  # noqa: E402
from pc.protocol import (CMD_BOTH, CMD_CNN, CMD_ECHO, CMD_INFO, CMD_SNN, SYNC_REQ,  # noqa: E402
                         build_response, crc16)


class SimBoard:
    def __init__(self, baud=4_000_000):
        import os
        os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")
        import tensorflow as tf
        sys.path.insert(0, str(ROOT / "snn"))
        import train_snn as ts
        import torch

        self.cnn = tf.keras.models.load_model(ROOT / "cnn" / "out" / "models" / "shape_cnn.keras")
        m = ts.SNNMLP()
        m.load_state_dict(torch.load(ROOT / "snn" / "out" / "models" / "snn_mlp.pt"))
        self.qw, self.int_forward = ts.quant(m), ts.int_forward
        self.baud, self.rx, self.tx = baud, b"", b""

    # -- pyserial-like interface --
    def reset_input_buffer(self):
        self.tx = b""

    def write(self, data):
        self.rx += data
        self._process()

    def read(self, n):
        out, self.tx = self.tx[:n], self.tx[n:]
        return out

    def close(self):
        pass

    # -- board logic --
    def _process(self):
        while True:
            i = self.rx.find(SYNC_REQ)
            if i < 0 or len(self.rx) < i + 5:
                return
            cmd, n = struct.unpack_from("<BH", self.rx, i + 2)
            end = i + 5 + n + 2
            if len(self.rx) < end:
                return
            body, crc = self.rx[i + 2:end - 2], struct.unpack_from("<H", self.rx, end - 2)[0]
            self.rx = self.rx[end:]
            if crc != crc16(body):
                self.tx += build_response(cmd, 1)
                continue
            self.tx += self._handle(cmd, body[3:])

    def _handle(self, cmd, payload):
        if cmd == CMD_ECHO:
            return build_response(cmd, 0, payload)
        if cmd == CMD_INFO:
            return build_response(cmd, 0, struct.pack("<BBIIH", 1, 1, self.baud, 325_000_000, 0x100))
        if cmd not in (CMD_CNN, CMD_SNN, CMD_BOTH):
            return build_response(cmd, 3)
        if len(payload) != IMG_H * IMG_W:
            return build_response(cmd, 2)
        img = np.frombuffer(payload, np.uint8).reshape(IMG_H, IMG_W)
        out = struct.pack("<I", int(len(payload) * 10 / self.baud * 1e6))
        if cmd != CMD_SNN:
            p = self.cnn.predict(cnn_normalize(img)[None, ..., None], verbose=0)[0]
            # emulate logits (log-probabilities) in Q8.8
            lg = [int(np.clip(np.log(max(v, 1e-6)) * 256, -32768, 32767)) for v in p]
            out += struct.pack("<BBII4i", 1, int(p.argmax()), 0, 0, *lg)
        if cmd != CMD_CNN:
            pred, lg = self.int_forward(self.qw, snn_features_int16(img)[None, None, :])
            out += struct.pack("<BBII4i", 2, int(pred[0]), 0, 0, *map(int, lg[0]))
        return build_response(cmd, 0, out)
