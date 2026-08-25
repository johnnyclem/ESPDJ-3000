# ESPDJ-3000
a cdj-3000 clone built for ma touch 1.28" display and esp32 

# ESP-DJ3000 — Modular Micro DJ System

**Design doc v0.2 — 2026-08-23 — merged (Claude v0.1 × Grok review)**

Three unit types, magnetically docked in a row, that self-discover and "just work":

```
┌──────────┐   ┌────────────┐   ┌──────────┐
│  DECK A  │═══│ CONTROLLER │═══│  DECK B  │
│ (rotary) │   │ (mix/tempo)│   │ (rotary) │
└──────────┘   └────────────┘   └──────────┘

Valid configs: [Deck] · [Deck|Deck] · [Deck|Ctrl] · [Ctrl|Deck] · [Deck|Ctrl|Deck]
+ wireless mode: two undocked decks paired over ESP-NOW
```

---

## 0. Decision matrix — where the two designs differed

| # | Feature / decision | Grok | Claude v0.1 | **Pick** | Why |
|---|---|---|---|---|---|
| 1 | Audio path | Local per deck; crossfade digital-only; "add analog mixing later" | Shared analog sum bus over pogo pins; crossfade is a control signal, gain applied per-deck | **Claude** | Two decks with independent jacks can't produce one mixed output without an external mixer — which defeats the product. Grok's scheme survives as the documented fallback mode if the bus fails the noise bench test (§6). |
| 2 | Headphone cue | Absent | Dedicated CUE_L/R bus (3rd pogo stack) | **Claude** | Pre-listen is what separates a DJ rig from a synced jukebox. |
| 3 | Data link | Full-duplex UART (TX+RX pins) + spare | Half-duplex multi-drop UART, 1 pin, 1 Mbps | **Claude** | Full-duplex point-to-point doesn't extend to a 3-unit shared bus (TX collision). Half-duplex with a master poll schedule does, and frees a pin. |
| 4 | Side/role detection | DETECT pin + resistor ID | Per-port SENSE + occupancy broadcast | **Merge** | Per-port SENSE determines topology; add Grok's resistor ID on SENSE so a unit knows its neighbor's *type* (deck vs controller) before the bus even comes up. |
| 5 | Power sharing | Negotiated: "strongest battery or USB-C becomes source" | Passive ideal-diode ORing | **Claude, clarified** | Passive ORing is automatic and can't deadlock. Policy: only USB-C-powered units drive VBUS; battery units never boost onto the bus (each runs itself from its own cell). |
| 6 | Battery/charging | Flagged that stock MaTouch lacks a full charger; add charge + protection | Hand-waved "LiPo cell" | **Grok** | Real catch. Add BQ24074 (power-path) or TP4056 + DW01 protection per unit. |
| 7 | Cue headphone drive | MAX98357A / codec + headphone amp option | PCM5102A line-level only | **Grok (adapted)** | PCM5102A can't drive 32Ω cans. Keep PCM5102A for the line-level program bus; add a small headphone amp (TPA6132/PAM8908) on the cue buffer. |
| 8 | Tempo processing | "Timestretch / pitch via ESP-ADF" | Varispeed only in v1; master tempo explicitly cut | **Claude** | Formant-preserving stretch on an S3 doing UI + WiFi + SD is an overpromise. Varispeed = Vinyl Mode = feature. |
| 9 | Stems | "Stems on SD" (no bandwidth analysis) | Single interleaved 8-ch WAV, sequential read, gated on SD bench test | **Claude** | Four separate files = seek thrash. The interleave trick is what makes stems even plausible. |
| 10 | Wireless deck-to-deck | ESP-NOW fallback / undocked pairing | Absent | **Grok** | Genuinely good: two decks across a table with no dock still sync and accept phone control. Also a graceful fallback if a pogo DATA contact gets flaky. |
| 11 | Beat/phase LEDs | WS2812 ring / under-glow on decks + RGB pad edge-light on controller | Absent | **Grok** | Cheap (1 GPIO), high playability value, matches the reference aesthetic. |
| 12 | Haptics | On cue points, beatgrid hits, buttons | Jog detents only | **Merge** | All of the above. |
| 13 | Touch gestures | Slip, beat-jump, key shift | Cue/loop/scrub | **Merge, minus key shift in v1** | Slip + beat-jump are cheap and great. Key shift is pitch-shifting DSP — same budget problem as #8, goes to v2. |
| 14 | Beat sync mechanism | Unspecified ("sync" messages) | Timestamped phase broadcast + resample-ratio trim (Link-style discipline) | **Claude** | Sync needs a clock-drift story, not just a sync button. |
| 15 | MIDI out, sampler mode | Future hooks | Absent | **Grok** | USB-MIDI is nearly free on the S3's native USB and makes each deck a Rekordbox/Traktor controller. v2 list. |
| 16 | Enclosure | ~70–80mm square, flush magnetic faces, matte white, rubber feet; print → CNC/injection | Absent | **Grok** | Adopted wholesale, §8. |
| 17 | Controller OLED | Optional tiny OLED | Pure control surface, no display | **Claude** | Deck displays + pad RGB already show all state; an OLED adds cost and firmware surface for nothing. |
| 18 | Web app hosting | Captive portal or mDNS | SoftAP + mDNS + WebSocket | **Merge** | Do both: captive portal for zero-typing onboarding, `dj.local` for repeat use. |

Everything below is the merged design.

---

## 1. Unit summary

**Deck** = MaTouch 1.28" ToolSet_Controller (ESP32-S3, 16MB flash, 8MB PSRAM, 240×240 round IPS + CST816S touch, rotary ring + center press, haptic motor, microSD, USB-C, WiFi/BT5) plus:
- PCM5102A I2S DAC (program audio, line level)
- TPA6132-class headphone amp on the cue tap
- LiPo 1000–2000mAh + BQ24074 power-path charger + protection
- Left + right magnetic pogo faces (3 stacks each, §2)
- WS2812 ring/under-glow for beat-phase indication
- 3.5mm master jack + 3.5mm cue jack

**Controller** = ESP32-C3 on custom PCB, pure control surface: crossfader, 2× 45mm tempo faders, 4+4 RGB-edge-lit pads, 2+2 encoders, 1+1 pots (17 controls). MCP23017 for pads/encoders, RC-filtered ADC + hysteresis for faders/pots. Audio bus passes through; its jacks are just more buffered taps. No display.

**Roles auto-assign:** two decks → Left/Right; deck+controller → deck takes the side it's docked on; all three → classic dual+mixer. Any hot-plug change re-enumerates in ~100ms without interrupting playback.

---

## 2. Interconnect — 9 pins (three 3-pin pogo stacks per face)

All lines bussed straight through each unit (left face ↔ right face in parallel), **except SENSE**, which is point-to-point per mate.

| Pin | Name | Purpose |
|-----|------|---------|
| 1 | GND | power/digital ground |
| 2 | VBUS | 5V share — USB-C-powered units drive it through ideal diodes; battery units draw nothing and give nothing |
| 3 | DATA | half-duplex multi-drop UART, 1 Mbps, master-polled |
| 4 | SENSE | neighbor detect + resistor-ID (deck=10k, controller=22k to GND) |
| 5 | AGND | audio return, star-joined to GND at one point per unit |
| 6 | AUDIO_L | program summing bus (line level, each deck sums via 1kΩ) |
| 7 | AUDIO_R | program summing bus |
| 8 | CUE_L | pre-fader cue bus |
| 9 | CUE_R | pre-fader cue bus |

Ferrites on VBUS at each face; local LDO per DAC. Any unit buffers AUDIO→master jack and CUE→headphone amp→cue jack, so master out and cue are available everywhere.

### Discovery
1. Mate → SENSE interrupt; ADC read of the ID resistor tells you *what* docked and on *which side* before any protocol runs.
2. Debounce 100ms → all units broadcast `{mac, type, leftOccupied, rightOccupied}` on DATA.
3. ≤3 units in a line ⇒ port occupancy fully determines order. Empty-left unit = leftmost.
4. Bus master = controller if present, else leftmost deck. 250Hz control frames (~8 bytes) → ~500× bus headroom.

### Wireless mode (undocked)
Two decks with no physical link pair over **ESP-NOW**: same control frames, same beat-sync messages, ~2–5ms typical latency. Each deck uses its own audio outs (this is the one mode where Grok's local-audio model applies, because there's no bus to sum on). Phone web app works identically.

### Beat sync
Master broadcasts timestamped beat-phase at 10Hz; slaves trim resample ratio ±0.02% to converge — Link-style clock discipline. Stretch: real Ableton Link over WiFi to bridge into the Neon Link ecosystem (the SoftAP multicast work transfers directly).

---

## 3. Deck firmware (ESP-IDF, dual core)

- **Core 1 (RT):** SD → PSRAM ring (4–8s) → cubic resampler (varispeed & scratch = signed ratio from jog velocity) → gain/EQ/filter → I2S DMA. Pre-fader tap → cue switch → CUE bus.
- **Core 0:** LVGL/SquareLine UI, touch + ring encoder, bus/ESP-NOW protocol, WiFi + web server when master, WS2812 beat ring, haptics.

**UI:** rotating waveform ring, center BPM/pitch/key/time, touch zones for cue/play/loop/hot-cues, slip mode, beat-jump, long-press → browser. Ring = jog with haptic detents; haptic ticks on cue points and downbeats.

**Media pipeline:**
- v1: 16-bit/44.1k WAV only. Zero on-device analysis.
- **Desktop prep CLI** (TS or Python): drops `track.wav` + `track.json` (BPM, beatgrid, key, cues, waveform overview) onto SD; imports Rekordbox XML.
- **Stems:** one interleaved 8-channel WAV per track (sequential read, deinterleave in PSRAM). ~706 KB/s — gated on the P0 SD bench test.

---

## 4. Phone-as-controller

Leftmost deck raises SoftAP `ESP-DJ` with captive portal (zero typing) + mDNS `dj.local` for repeat visits. Single-file SPA from LittleFS, WebSocket at 30Hz throttle, relayed to the partner deck over DATA or ESP-NOW. Latency ~20–60ms: fine for faders/hot cues; scratching never leaves the deck's own encoder, so it doesn't care.

---

## 5. Controller detail

Layout per the reference image: 2+2 encoders top, 4+4 edge-lit pads flanking, tempo faders center, crossfader below, pots outboard. Pads address the deck on their side (hot cue / loop / FX / shift layer). Crossfader broadcasts position; each deck computes its own gain curve (constant-power, with a curve setting). Controller can power the whole chain from its USB-C.

---

## 6. Reality checks (unchanged, still load-bearing)

1. **Master tempo & key shift are v2.** Varispeed ships as Vinyl Mode.
2. **P0 bench: SD sustained sequential read** on the MaTouch's actual microSD wiring. Decides stems.
3. **P0 bench: analog bus noise** — two boards + summing network on the bench before any enclosure. If it fails: fallback is Grok's local-audio model (each deck's own jack, digital crossfade), and the AUDIO/CUE pins get repurposed — but test first, because the sum bus is what makes the product.
4. No codecs, no recording, no on-device analysis, no FX beyond filter/EQ in v1.

## 7. Build phases

- **P0 (bench weekend):** WAV → resampler → PCM5102A; measure SD throughput; two-board analog sum + noise floor listen test. Two go/no-gos.
- **P1 — one deck standalone:** full UI, varispeed, cue/loop/slip/beat-jump, haptics, LED ring, prep CLI. Already an instrument.
- **P2 — two decks:** pogo faces, discovery, sum bus, beat sync, ESP-NOW mode, phone web app.
- **P3 — controller:** PCB + panel, full triptych.
- **P4:** stems, Ableton Link bridge, USB-MIDI controller mode, sampler mode, FX, key shift.

## 8. Enclosure

- Deck: 70–80mm square, rounded corners, matte white / light gray, flush magnetic side faces, rubber feet; display + ring proud of the top face.
- Controller: same height and depth, width to fit the 17 controls (~120–140mm); soft rectangular pads with colored edge illumination per the reference image.
- 3D print (matte PLA/resin) → CNC or injection if it graduates past friends-and-family.

## 9. BOM sketch (full 3-unit system)

| Item | Qty | ~Cost |
|------|-----|-------|
| MaTouch 1.28" ToolSet_Controller | 2 | $44 |
| PCM5102A DAC module | 2 | $6 |
| TPA6132/PAM8908 headphone amp | 2 | $4 |
| BQ24074 (or TP4056+DW01) charge/protect | 3 | $6 |
| LiPo 1500mAh | 3 | $18 |
| ESP32-C3 module | 1 | $3 |
| 3-pin magnetic pogo pairs | 12 | on hand |
| Faders ×3, pads ×8, encoders ×4, pots ×2 | — | ~$25 |
| WS2812 rings/strips | — | ~$5 |
| Op-amps, LDOs, ferrites, jacks, passives | — | ~$14 |
| Interconnect + controller PCBs (JLC) | — | ~$15 |
| **Total** | | **~$140 + printed enclosures** |


---

# Software implementation

Everything below §0-§9 describes the design; this section describes the code
that implements it.

```
common/                  Portable C99 core, shared by both firmwares and the
                         host test suite: wire protocol + topology resolution
                         (espdj_proto), cubic varispeed resampler
                         (espdj_resampler), SPSC audio ring (espdj_ringbuf),
                         EQ/filter/crossfader DSP (espdj_mix), Link-style
                         beat-sync discipline (espdj_sync).
firmware/deck/           ESP-IDF (>= 5.2) project for the MaTouch 1.28"
                         ToolSet_Controller (ESP32-S3). Core 1: SD feeder +
                         RT audio pipeline. Core 0: LVGL UI + jog ring,
                         SENSE/bus discovery, ESP-NOW pairing, SoftAP +
                         captive portal + dj.local + WebSocket, WS2812 beat
                         ring, haptics.
firmware/controller/     ESP-IDF project for the ESP32-C3 control surface:
                         MCP23017 pads/encoders, RC+hysteresis ADC faders,
                         bus master (250 Hz control frames, deck polling),
                         pad edge lighting.
tools/prep/              Desktop prep CLI (`espdj-prep`, Python): WAV ->
                         track.json sidecars (BPM, beatgrid anchor, key,
                         cues, ring-waveform overview), Rekordbox XML
                         import, 8-channel stems pack/unpack.
webapp/index.html        Single-file phone SPA served from the deck's
                         LittleFS (WebSocket, 30 Hz throttle).
tests/host/              gcc unit tests for everything in common/.
```

## Building & testing

Host tests (no hardware needed):

```sh
make -C tests/host test           # protocol, DSP, sync discipline
pip install numpy pytest
python -m pytest tools/prep/tests # prep CLI
```

Deck firmware (ESP-IDF ≥ 5.2 installed and exported):

```sh
cd firmware/deck
idf.py set-target esp32s3
idf.py build flash monitor
```

Controller firmware:

```sh
cd firmware/controller
idf.py set-target esp32c3
idf.py build flash monitor
```

Web app → deck LittleFS partition:

```sh
pip install littlefs-python
littlefs-python create webapp lfs.bin -v --fs-size=0x100000
# flash lfs.bin at the littlefs partition offset shown by idf.py partition-table
```

Track prep:

```sh
cd tools/prep && pip install -e .
espdj-prep analyze /Volumes/SDCARD            # sidecars for every WAV
espdj-prep analyze --rekordbox rekordbox.xml /Volumes/SDCARD
espdj-prep stems pack drums.wav bass.wav melody.wav vocals.wav -o track.8ch.wav
```

## Status vs. the build phases (§7)

- **P0** — SD sequential-read bench ships in the deck firmware
  (`sd_card_bench_kbps`, logged at boot with go/no-go verdicts for single
  stream and stems). The analog-bus noise bench is hardware work.
- **P1** — implemented: full RT audio path (feeder → PSRAM cache → cubic
  resampler → EQ/filter → I2S), varispeed/scratch from the jog ring,
  cue/hot-cue/loop/slip/beat-jump, haptics, LED beat ring, LVGL player +
  browser, prep CLI.
- **P2** — implemented: SENSE discovery + resistor ID, half-duplex 1-wire
  bus with master polling, topology/role auto-assignment, crossfader as a
  control signal, 10 Hz timestamped beat-phase sync (±0.02% trim), ESP-NOW
  undocked pairing, phone web app.
- **P3** — implemented: controller firmware (scan, bus master, pad LEDs).
- **P4** — stems groundwork only (prep-side interleave + 8-ch WAV reader on
  the deck); Link bridge, USB-MIDI, sampler, FX, key shift remain open.

Pin maps in `firmware/*/main/pins.h` are provisional for the expansion
wiring — verify against your board revision before first flash. The
firmware compiles against ESP-IDF ≥ 5.2 with the managed components in
`idf_component.yml`; only the portable core and prep CLI are covered by
the CI test suite (no hardware in the loop).
