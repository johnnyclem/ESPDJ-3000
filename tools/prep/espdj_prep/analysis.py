"""Track analysis: BPM, beatgrid anchor, musical key.

Deliberately plain-numpy DSP (no librosa dependency) since the targets are
4/4 dance tracks with a steady grid — the case the ESP-DJ3000 is built for.

Pipeline:
  1. onset envelope: half-wave-rectified spectral flux of an STFT
  2. tempo: autocorrelation of the envelope over the 60-200 BPM lag range,
     with octave folding preference into 70-180
  3. beatgrid anchor: comb-correlate a beat impulse train against the
     envelope to find the phase offset of beat 1
  4. key: pitch-class chromagram correlated against Krumhansl-Schmuckler
     major/minor profiles; reported in standard + Camelot notation
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

FRAME = 2048
HOP = 512

NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

# Krumhansl-Schmuckler key profiles
KS_MAJOR = np.array([6.35, 2.23, 3.48, 2.33, 4.38, 4.09,
                     2.52, 5.19, 2.39, 3.66, 2.29, 2.88])
KS_MINOR = np.array([6.33, 2.68, 3.52, 5.38, 2.60, 3.53,
                     2.54, 4.75, 3.98, 2.69, 3.34, 3.17])

CAMELOT = {
    ("B", True): "1B", ("F#", True): "2B", ("C#", True): "3B",
    ("G#", True): "4B", ("D#", True): "5B", ("A#", True): "6B",
    ("F", True): "7B", ("C", True): "8B", ("G", True): "9B",
    ("D", True): "10B", ("A", True): "11B", ("E", True): "12B",
    ("G#", False): "1A", ("D#", False): "2A", ("A#", False): "3A",
    ("F", False): "4A", ("C", False): "5A", ("G", False): "6A",
    ("D", False): "7A", ("A", False): "8A", ("E", False): "9A",
    ("B", False): "10A", ("F#", False): "11A", ("C#", False): "12A",
}


@dataclass
class Analysis:
    bpm: float
    first_beat_s: float
    key: str          # e.g. "Am"
    camelot: str      # e.g. "8A"


def _stft_mag(x: np.ndarray) -> np.ndarray:
    """Magnitude STFT, frames x bins."""
    n = (len(x) - FRAME) // HOP + 1
    if n < 4:
        raise ValueError("track too short to analyze")
    idx = np.arange(FRAME)[None, :] + HOP * np.arange(n)[:, None]
    frames = x[idx] * np.hanning(FRAME)[None, :]
    return np.abs(np.fft.rfft(frames, axis=1))


def onset_envelope(x: np.ndarray) -> np.ndarray:
    mag = _stft_mag(x)
    flux = np.diff(mag, axis=0)
    flux[flux < 0] = 0.0
    env = flux.sum(axis=1)
    env -= env.mean()
    if env.std() > 0:
        env /= env.std()
    return env


def detect_bpm(x: np.ndarray, rate: int) -> float:
    env = onset_envelope(x)
    fps = rate / HOP  # envelope frames per second

    ac = np.correlate(env, env, mode="full")[len(env) - 1:]
    lag_min = int(fps * 60 / 200)  # 200 BPM
    lag_max = int(fps * 60 / 60)   # 60 BPM
    lag_max = min(lag_max, len(ac) - 1)
    if lag_max <= lag_min:
        return 120.0
    window = ac[lag_min:lag_max + 1]
    lag = lag_min + int(np.argmax(window))
    bpm = 60.0 * fps / lag

    # fold into the 70-180 club range
    while bpm < 70:
        bpm *= 2
    while bpm > 180:
        bpm /= 2

    # refine with parabolic interpolation around the (possibly folded) lag
    lag = 60.0 * fps / bpm
    li = int(round(lag))
    if 1 <= li < len(ac) - 1:
        a, b, c = ac[li - 1], ac[li], ac[li + 1]
        denom = a - 2 * b + c
        if abs(denom) > 1e-12:
            delta = 0.5 * (a - c) / denom
            if abs(delta) < 1:
                bpm = 60.0 * fps / (li + delta)
    return float(round(bpm, 2))


def detect_first_beat(x: np.ndarray, rate: int, bpm: float) -> float:
    """Phase of the beatgrid: offset in seconds of the first beat."""
    env = onset_envelope(x)
    fps = rate / HOP
    period = fps * 60.0 / bpm
    n_offsets = max(int(period), 1)
    scores = np.zeros(n_offsets)
    for off in range(n_offsets):
        idx = np.arange(off, len(env), period).astype(int)
        idx = idx[idx < len(env)]
        if len(idx):
            scores[off] = env[idx].mean()
    best = int(np.argmax(scores))
    # envelope frame k spans samples [k*HOP, k*HOP+FRAME); onset lands
    # roughly at the frame start after the diff, offset by one frame
    return float((best + 1) * HOP / rate)


def detect_key(x: np.ndarray, rate: int) -> tuple[str, str]:
    mag = _stft_mag(x)
    freqs = np.fft.rfftfreq(FRAME, 1.0 / rate)
    valid = (freqs >= 110.0) & (freqs < 3000.0)
    m = mag[:, valid]
    # Keep only spectral-peak bins per frame: tonal partials survive,
    # broadband transients (kicks, clicks) don't pollute the chroma.
    peaks = (m[:, 1:-1] > m[:, :-2]) & (m[:, 1:-1] > m[:, 2:])
    tonal = np.zeros_like(m)
    tonal[:, 1:-1] = np.where(peaks, m[:, 1:-1], 0.0)

    midi = 69 + 12 * np.log2(freqs[valid] / 440.0)
    pc = np.round(midi).astype(int) % 12
    spectrum = tonal.mean(axis=0)
    chroma = np.zeros(12)
    for k in range(12):
        chroma[k] = spectrum[pc == k].sum()
    if chroma.sum() > 0:
        chroma /= chroma.sum()

    best_score, best_key, best_major = -2.0, "C", True
    for tonic in range(12):
        prof_maj = np.roll(KS_MAJOR, tonic)
        prof_min = np.roll(KS_MINOR, tonic)
        s_maj = float(np.corrcoef(chroma, prof_maj)[0, 1])
        s_min = float(np.corrcoef(chroma, prof_min)[0, 1])
        if s_maj > best_score:
            best_score, best_key, best_major = s_maj, NOTE_NAMES[tonic], True
        if s_min > best_score:
            best_score, best_key, best_major = s_min, NOTE_NAMES[tonic], False

    name = best_key if best_major else best_key + "m"
    camelot = CAMELOT.get((best_key, best_major), "")
    return name, camelot


def analyze(x: np.ndarray, rate: int) -> Analysis:
    bpm = detect_bpm(x, rate)
    first_beat = detect_first_beat(x, rate, bpm)
    key, camelot = detect_key(x, rate)
    return Analysis(bpm=bpm, first_beat_s=round(first_beat, 4),
                    key=key, camelot=camelot)
