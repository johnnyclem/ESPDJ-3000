"""espdj-prep — desktop prep CLI (design doc §3, "Desktop prep CLI").

Usage:
  espdj-prep analyze TRACK.wav [...]      write TRACK.json sidecars
  espdj-prep analyze --rekordbox rb.xml DIR|TRACK.wav [...]
                                          prefer Rekordbox grid/cues where a
                                          track matches, analyze the rest
  espdj-prep stems pack drums.wav bass.wav melody.wav vocals.wav -o t.8ch.wav
  espdj-prep stems unpack t.8ch.wav -o outprefix
  espdj-prep show TRACK.wav               print the sidecar (or analyze dry)
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from . import analysis, rekordbox, stems, waveform
from .wavio import read_wav

MAX_CUES = 8


def _iter_wavs(paths: list[str]):
    for p in paths:
        path = Path(p)
        if path.is_dir():
            yield from sorted(
                q for q in path.glob("*.wav")
                if not q.name.endswith(".8ch.wav")
            )
        else:
            yield path


def _sidecar(path: Path) -> Path:
    return path.with_suffix(".json")


def cmd_analyze(args: argparse.Namespace) -> int:
    rb_tracks = rekordbox.parse(args.rekordbox) if args.rekordbox else []
    failures = 0
    for wav_path in _iter_wavs(args.tracks):
        try:
            w = read_wav(str(wav_path))
            mono = w.mono()

            meta = {
                "title": wav_path.stem,
                "artist": "",
                "beats_per_bar": 4,
                "has_stems": wav_path.with_name(
                    wav_path.stem + ".8ch.wav").exists(),
            }

            rb = rekordbox.match_for(str(wav_path), rb_tracks)
            if rb and rb.bpm:
                meta["title"] = rb.name or wav_path.stem
                meta["artist"] = rb.artist
                meta["bpm"] = rb.bpm
                meta["first_beat_s"] = rb.first_beat_s or 0.0
                meta["key"] = rb.tonality
                meta["cues"] = rb.cues[:MAX_CUES]
                source = "rekordbox"
            else:
                a = analysis.analyze(mono, w.rate)
                meta["bpm"] = a.bpm
                meta["first_beat_s"] = a.first_beat_s
                meta["key"] = a.camelot or a.key
                meta["cues"] = [a.first_beat_s]
                source = "analysis"

            meta["cues"] = (meta["cues"] + [-1.0] * MAX_CUES)[:MAX_CUES]
            meta["overview"] = waveform.overview(mono)

            out = _sidecar(wav_path)
            out.write_text(json.dumps(meta, indent=1))
            print(f"{wav_path.name}: {meta['bpm']:.2f} BPM, "
                  f"key {meta['key'] or '?'}, anchor {meta['first_beat_s']:.3f}s "
                  f"[{source}] -> {out.name}")
        except Exception as e:  # keep batch going, report at the end
            failures += 1
            print(f"{wav_path.name}: FAILED ({e})", file=sys.stderr)
    return 1 if failures else 0


def cmd_stems(args: argparse.Namespace) -> int:
    if args.action == "pack":
        if len(args.files) != 4:
            print("stems pack needs 4 stereo WAVs: drums bass melody vocals",
                  file=sys.stderr)
            return 1
        stems.pack(args.files, args.output)
        print(f"packed -> {args.output}")
    else:
        if len(args.files) != 1:
            print("stems unpack takes one .8ch.wav", file=sys.stderr)
            return 1
        for p in stems.unpack(args.files[0], args.output):
            print(f"unpacked -> {p}")
    return 0


def cmd_show(args: argparse.Namespace) -> int:
    path = Path(args.track)
    sidecar = _sidecar(path)
    if sidecar.exists():
        print(sidecar.read_text())
        return 0
    w = read_wav(str(path))
    a = analysis.analyze(w.mono(), w.rate)
    print(json.dumps({"bpm": a.bpm, "first_beat_s": a.first_beat_s,
                      "key": a.key, "camelot": a.camelot}, indent=1))
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="espdj-prep",
                                 description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    a = sub.add_parser("analyze", help="write track.json sidecars")
    a.add_argument("tracks", nargs="+", help="WAV files or directories")
    a.add_argument("--rekordbox", help="rekordbox.xml to import grids/cues from")
    a.set_defaults(fn=cmd_analyze)

    s = sub.add_parser("stems", help="pack/unpack 8-channel stems WAV")
    s.add_argument("action", choices=["pack", "unpack"])
    s.add_argument("files", nargs="+")
    s.add_argument("-o", "--output", required=True,
                   help="output file (pack) or prefix (unpack)")
    s.set_defaults(fn=cmd_stems)

    sh = sub.add_parser("show", help="print a track's sidecar / quick analysis")
    sh.add_argument("track")
    sh.set_defaults(fn=cmd_show)

    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
