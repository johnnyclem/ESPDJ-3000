"""ESP-DJ3000 desktop prep tool.

The decks do zero on-device analysis (v1 media policy): everything a deck
needs to know about a track — BPM, beatgrid anchor, key, cues, waveform
overview — is computed here and dropped next to the WAV as `track.json`.
"""

__version__ = "0.1.0"
