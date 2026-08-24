import json
import math
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from espdj_prep import analysis, rekordbox, stems, waveform  # noqa: E402
from espdj_prep.cli import main as cli_main  # noqa: E402
from espdj_prep.wavio import read_wav, write_wav  # noqa: E402

RATE = 44100


def synth_track(bpm=128.0, seconds=20.0, first_beat=0.25, key_tonic=9,
                minor=True, rate=RATE):
    """Kick on every beat + a tonal loop outlining a triad (default Am)."""
    n = int(seconds * rate)
    t = np.arange(n) / rate
    x = np.zeros(n, dtype=np.float64)

    # kicks: decaying 55 Hz thumps with a click transient on the grid
    period = 60.0 / bpm
    beat = first_beat
    while beat < seconds - 0.1:
        i0 = int(beat * rate)
        dur = int(0.09 * rate)
        tt = np.arange(dur) / rate
        x[i0:i0 + dur] += 0.9 * np.exp(-tt * 35) * np.sin(2 * np.pi * 55 * tt)
        x[i0:i0 + int(0.004 * rate)] += 0.5  # click for spectral flux
        beat += period

    # sustained triad (root, minor third, fifth) for the key detector
    root_midi = 57 + (key_tonic - 9) % 12  # A3 by default
    intervals = (0, 3, 7) if minor else (0, 4, 7)
    for iv in intervals:
        f = 440.0 * 2 ** ((root_midi + iv - 69) / 12)
        x += 0.15 * np.sin(2 * np.pi * f * t)

    x = np.clip(x, -1, 1)
    return (x * 32000).astype(np.int16)


@pytest.fixture(scope="module")
def track_wav(tmp_path_factory):
    d = tmp_path_factory.mktemp("tracks")
    path = d / "testtrack.wav"
    mono = synth_track()
    write_wav(str(path), RATE, np.stack([mono, mono], axis=1))
    return path


def test_bpm_detection(track_wav):
    w = read_wav(str(track_wav))
    bpm = analysis.detect_bpm(w.mono(), w.rate)
    assert abs(bpm - 128.0) < 1.0


def test_first_beat(track_wav):
    w = read_wav(str(track_wav))
    mono = w.mono()
    bpm = analysis.detect_bpm(mono, w.rate)
    fb = analysis.detect_first_beat(mono, w.rate, bpm)
    period = 60.0 / 128.0
    # anchor may land on any grid line; check phase distance to the grid
    phase = (fb - 0.25) % period
    assert min(phase, period - phase) < 0.05


def test_key_detection(track_wav):
    w = read_wav(str(track_wav))
    key, camelot = analysis.detect_key(w.mono(), w.rate)
    assert key == "Am"
    assert camelot == "8A"


def test_key_detection_major():
    mono = synth_track(key_tonic=0, minor=False, seconds=8.0)  # C major
    key, camelot = analysis.detect_key(mono / 32768.0, RATE)
    assert key == "C"
    assert camelot == "8B"


def test_overview_shape_and_range(track_wav):
    w = read_wav(str(track_wav))
    ov = waveform.overview(w.mono())
    assert len(ov) == 240
    assert max(ov) == 255
    assert all(0 <= v <= 255 for v in ov)


def test_cli_analyze_writes_sidecar(track_wav):
    rc = cli_main(["analyze", str(track_wav)])
    assert rc == 0
    sidecar = track_wav.with_suffix(".json")
    meta = json.loads(sidecar.read_text())
    assert abs(meta["bpm"] - 128.0) < 1.0
    assert meta["key"] == "8A"
    assert len(meta["overview"]) == 240
    assert len(meta["cues"]) == 8
    assert meta["beats_per_bar"] == 4


def test_stems_roundtrip(tmp_path):
    paths = []
    rng = np.random.default_rng(7)
    for name in stems.STEM_NAMES:
        data = (rng.standard_normal((RATE, 2)) * 8000).astype(np.int16)
        p = tmp_path / f"{name}.wav"
        write_wav(str(p), RATE, data)
        paths.append(str(p))

    packed = tmp_path / "track.8ch.wav"
    stems.pack(paths, str(packed))
    w = read_wav(str(packed))
    assert w.channels == 8
    assert w.frames == RATE

    outs = stems.unpack(str(packed), str(tmp_path / "out"))
    for src, dst in zip(paths, outs):
        assert np.array_equal(read_wav(src).data, read_wav(dst).data)

    # sequential-read bandwidth claim from the design doc: 8ch 16-bit 44.1k
    assert 8 * 2 * RATE / 1024 == pytest.approx(689, abs=1)  # ~706 KB/s dec.


REKORDBOX_XML = """<?xml version="1.0" encoding="UTF-8"?>
<DJ_PLAYLISTS Version="1.0.0">
 <COLLECTION Entries="2">
  <TRACK TrackID="1" Name="Test Track" Artist="Unit Test"
         Location="file://localhost/Users/dj/Music/testtrack.wav"
         AverageBpm="127.98" Tonality="Am">
   <TEMPO Inizio="0.252" Bpm="128.00" Metro="4/4" Battito="1"/>
   <POSITION_MARK Name="" Type="0" Start="0.252" Num="0"/>
   <POSITION_MARK Name="" Type="0" Start="30.252" Num="1"/>
   <POSITION_MARK Name="mem" Type="0" Start="15.0" Num="-1"/>
  </TRACK>
  <TRACK TrackID="2" Name="Other" Artist="X"
         Location="file://localhost/Users/dj/Music/other.wav"
         AverageBpm="140.00" Tonality="F"/>
 </COLLECTION>
</DJ_PLAYLISTS>
"""


def test_rekordbox_import(tmp_path, track_wav):
    xml = tmp_path / "rekordbox.xml"
    xml.write_text(REKORDBOX_XML)
    tracks = rekordbox.parse(str(xml))
    assert len(tracks) == 2

    m = rekordbox.match_for(str(track_wav), tracks)
    assert m is not None
    assert m.bpm == 128.0
    assert m.first_beat_s == pytest.approx(0.252)
    assert m.cues == [0.252, 30.252]  # memory cue (-1) excluded
    assert m.tonality == "Am"

    rc = cli_main(["analyze", "--rekordbox", str(xml), str(track_wav)])
    assert rc == 0
    meta = json.loads(track_wav.with_suffix(".json").read_text())
    assert meta["bpm"] == 128.0
    assert meta["artist"] == "Unit Test"
    assert meta["first_beat_s"] == pytest.approx(0.252)


def test_cli_stems_commands(tmp_path):
    files = []
    for name in stems.STEM_NAMES:
        p = tmp_path / f"{name}.wav"
        write_wav(str(p), RATE,
                  np.ones((100, 2), dtype=np.int16) * 100)
        files.append(str(p))
    out = tmp_path / "x.8ch.wav"
    assert cli_main(["stems", "pack", *files, "-o", str(out)]) == 0
    assert cli_main(["stems", "unpack", str(out), "-o",
                     str(tmp_path / "roundtrip")]) == 0
    assert (tmp_path / "roundtrip.vocals.wav").exists()
