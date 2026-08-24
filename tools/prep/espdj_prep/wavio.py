"""Minimal WAV I/O on top of the stdlib `wave` module + numpy.

Only what the ESP-DJ3000 toolchain needs: 16-bit PCM in, 16-bit PCM out,
arbitrary channel counts (the stems interleave uses 8).
"""

from __future__ import annotations

import wave
from dataclasses import dataclass

import numpy as np


@dataclass
class Wav:
    rate: int
    data: np.ndarray  # shape (frames, channels), int16

    @property
    def frames(self) -> int:
        return self.data.shape[0]

    @property
    def channels(self) -> int:
        return self.data.shape[1]

    @property
    def duration_s(self) -> float:
        return self.frames / self.rate

    def mono(self) -> np.ndarray:
        """Float32 mono mix in [-1, 1]."""
        return self.data.astype(np.float32).mean(axis=1) / 32768.0


def read_wav(path: str) -> Wav:
    with wave.open(path, "rb") as w:
        if w.getsampwidth() != 2:
            raise ValueError(f"{path}: only 16-bit PCM WAV is supported")
        rate = w.getframerate()
        channels = w.getnchannels()
        raw = w.readframes(w.getnframes())
    data = np.frombuffer(raw, dtype="<i2").reshape(-1, channels)
    return Wav(rate=rate, data=data)


def write_wav(path: str, rate: int, data: np.ndarray) -> None:
    if data.dtype != np.int16:
        raise ValueError("write_wav expects int16 samples")
    if data.ndim == 1:
        data = data[:, None]
    with wave.open(path, "wb") as w:
        w.setnchannels(data.shape[1])
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(np.ascontiguousarray(data).astype("<i2").tobytes())
