"""Stems interleaving (design doc §0 #9, §3).

Four separate stem files would seek-thrash a microSD; one interleaved
8-channel WAV reads sequentially at ~706 KB/s, which is what makes stems
even plausible on the deck. Channel order (stereo pairs):

    0-1 drums · 2-3 bass · 4-5 melody · 6-7 vocals

Pack:   4 stereo WAVs -> track.8ch.wav
Unpack: track.8ch.wav -> 4 stereo WAVs (for verification)
"""

from __future__ import annotations

import numpy as np

from .wavio import Wav, read_wav, write_wav

STEM_NAMES = ["drums", "bass", "melody", "vocals"]


def pack(stem_paths: list[str], out_path: str) -> None:
    if len(stem_paths) != 4:
        raise ValueError("stems pack needs exactly 4 stereo WAVs "
                         f"({', '.join(STEM_NAMES)})")
    stems = [read_wav(p) for p in stem_paths]
    rate = stems[0].rate
    for p, s in zip(stem_paths, stems):
        if s.rate != rate:
            raise ValueError(f"{p}: sample rate {s.rate} != {rate}")
        if s.channels != 2:
            raise ValueError(f"{p}: stems must be stereo, got {s.channels}ch")

    frames = max(s.frames for s in stems)
    out = np.zeros((frames, 8), dtype=np.int16)
    for i, s in enumerate(stems):
        out[: s.frames, i * 2 : i * 2 + 2] = s.data
    write_wav(out_path, rate, out)


def unpack(in_path: str, out_prefix: str) -> list[str]:
    w = read_wav(in_path)
    if w.channels != 8:
        raise ValueError(f"{in_path}: expected 8 channels, got {w.channels}")
    paths = []
    for i, name in enumerate(STEM_NAMES):
        path = f"{out_prefix}.{name}.wav"
        write_wav(path, w.rate, np.ascontiguousarray(w.data[:, i * 2 : i * 2 + 2]))
        paths.append(path)
    return paths


def stereo_mix(w: Wav) -> np.ndarray:
    """Fold an 8-channel stems file to a stereo int16 preview mix."""
    if w.channels != 8:
        raise ValueError("stereo_mix expects an 8-channel stems file")
    acc = w.data.astype(np.int32)
    left = acc[:, 0] + acc[:, 2] + acc[:, 4] + acc[:, 6]
    right = acc[:, 1] + acc[:, 3] + acc[:, 5] + acc[:, 7]
    mix = np.stack([left, right], axis=1)
    return np.clip(mix, -32768, 32767).astype(np.int16)
