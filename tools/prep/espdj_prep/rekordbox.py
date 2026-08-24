"""Rekordbox collection XML import.

Pulls per-track BPM, beatgrid anchor, hot cues and tags from an exported
`rekordbox.xml` so an existing library preps without re-analysis. Matched
by audio file name (case-insensitive stem), since the SD card layout is
flat while Rekordbox stores absolute file:// URLs.
"""

from __future__ import annotations

import urllib.parse
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class RekordboxTrack:
    name: str
    artist: str
    location: str          # decoded absolute path from the XML
    bpm: float | None
    first_beat_s: float | None
    tonality: str
    cues: list[float] = field(default_factory=list)


def _decode_location(loc: str) -> str:
    # e.g. file://localhost/Users/dj/Music/track.wav
    parsed = urllib.parse.urlparse(loc)
    return urllib.parse.unquote(parsed.path)


def parse(xml_path: str) -> list[RekordboxTrack]:
    root = ET.parse(xml_path).getroot()
    tracks = []
    for t in root.iter("TRACK"):
        loc = t.get("Location")
        if not loc:
            continue  # playlist nodes also use TRACK tags, without Location
        bpm = None
        first_beat = None
        # first TEMPO node carries the grid anchor (Inizio = seconds)
        tempo = t.find("TEMPO")
        if tempo is not None:
            try:
                bpm = float(tempo.get("Bpm", ""))
                first_beat = float(tempo.get("Inizio", ""))
            except ValueError:
                pass
        if bpm is None:
            try:
                bpm = float(t.get("AverageBpm", ""))
            except (TypeError, ValueError):
                bpm = None

        cues = []
        for mark in t.findall("POSITION_MARK"):
            # Num >= 0 are hot cues; -1 is the memory cue
            try:
                if int(mark.get("Num", "-1")) >= 0:
                    cues.append(float(mark.get("Start", "")))
            except ValueError:
                continue

        tracks.append(RekordboxTrack(
            name=t.get("Name", ""),
            artist=t.get("Artist", ""),
            location=_decode_location(loc),
            bpm=bpm,
            first_beat_s=first_beat,
            tonality=t.get("Tonality", ""),
            cues=sorted(cues),
        ))
    return tracks


def match_for(wav_path: str, tracks: list[RekordboxTrack]) -> RekordboxTrack | None:
    stem = Path(wav_path).stem.lower()
    for t in tracks:
        if Path(t.location).stem.lower() == stem:
            return t
    return None
