"""Waveform overview for the deck's ring display: 240 bins, 0..255."""

from __future__ import annotations

import numpy as np

OVERVIEW_BINS = 240


def overview(x: np.ndarray, bins: int = OVERVIEW_BINS) -> list[int]:
    """Per-bin peak level of a mono float signal, normalized to 0..255."""
    if len(x) == 0:
        return [0] * bins
    # pad to a multiple of bins, then per-bin max of |x|
    n = int(np.ceil(len(x) / bins)) * bins
    padded = np.zeros(n, dtype=np.float32)
    padded[: len(x)] = np.abs(x)
    peaks = padded.reshape(bins, -1).max(axis=1)
    top = float(peaks.max())
    if top > 0:
        peaks = peaks / top
    return [int(round(p * 255)) for p in peaks]
