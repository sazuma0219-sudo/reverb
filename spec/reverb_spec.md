# REVERB specification

2026-11-26 · Shota Azuma

(Japanese version: [reverb_spec.ja.md](reverb_spec.ja.md))

A stereo multi-reverb for [AE Modular](https://www.tangiblewaves.com/). An RP2350 computes the reverb, and the MODE button switches between four modes: Plate, Hall, Room and Shimmer. The DRY signal passes through in analog at unity gain; only the WET signal is made digitally and added to it.

This document describes board **v1.0.1** and firmware **v0.22a**.

## 1. Overview

| Item | Value |
|---|---|
| Power | AE bus 5 V only. About 75 mA (measured) |
| Microcontroller | Waveshare RP2350-Zero (RP2350A, 150 MHz) |
| ADC / DAC | PCM1808 / PCM5102A, 24 bit, fs = 48 828.125 Hz |
| Latency | DRY: none (analog). WET conversion and processing: about 3–4 ms (estimate, PRE-DELAY at 0) |
| Modes | Plate / Hall / Room / Shimmer. The selected mode is kept when the power is off |

### Inputs and outputs

| Type | Name | Description |
|---|---|---|
| Input | L IN / R IN | Audio. AE standard level 1–4 V (centred on 2.5 V). DC coupled, about 100 kΩ input impedance. Unpatched inputs sit at 2.5 V (silence) |
| Input | CV1 / CV2 / CV3 | 0–5 V. Added to the WET / DECAY / TONE knob positions |
| Output | L MIX / R MIX | DRY + WET |
| Output | L WET / R WET | WET only |
| Output | NC | Not connected (spare pin of the 1×5 socket) |

### Panel

| Knob (top to bottom) | Function |
|---|---|
| WET | Amount of WET, on both the MIX and the WET outputs. Linear; at 100% the WET RMS is about equal to the input RMS |
| PRE-DELAY | 0–200 ms (square of the knob position) |
| DECAY | Reverb time. The range depends on the mode (section 5) |
| TONE | Brightness (distance in Room). Section 5 |
| CHARACTER | Depends on the mode (section 5) |

- Each press of MODE steps 0 → 1 → 2 → 3 → 0. The two LEDs show the number in binary (LED1 is the high bit).

| No. | LED1 LED2 | Mode |
|---|---|---|
| 0 | off off | Plate |
| 1 | off on | Hall |
| 2 | on off | Room |
| 3 | on on | Shimmer |

- In Plate both LEDs are off, so the LEDs do not show whether the module is powered.

## 2. Hardware

### 2.1 Structure

```
L IN ─[1k]─┬─[100k]─ VREF2.5                     (R is the same)
           └─ follower ─┬────────────────────────────────┐ DRY_L
                        └─[1µF]─[33k]─┬─ PCM1808          │
                                    [100pF]                │
                                      GND                  │
                              I2S ↕ RP2350-Zero            │
          PCM5102A ─[470Ω]─┬─ inverting ×(−1), 2.5 V centre ─┬─[470Ω]─ L WET
                         [2.2nF]                             │ WET_L
                           GND                               ▼
                      DRY_L ─[10k]─┐
                                   ├─ non-inverting sum (×2, ref. VREF2.5) ─[470Ω]─ L MIX
                      WET_L ─[10k]─┘   out = DRY + WET − 2.5 V
```

| Decision | Reason |
|---|---|
| DRY passes in analog at unity; only WET is added | No digital latency or quantisation noise on the DRY. No dual-gang pot needed |
| MCP6002 × 4 op-amps | Rail-to-rail input and output on a single 5 V supply |
| MIX is a non-inverting sum | The DRY polarity is kept, so mixing it with the original elsewhere does not cancel |
| Inverting WET output stage | Same circuit as WT VCO. The firmware inverts the sign before the DAC |
| Audio 3.3 V made on the board by an XC6206 | The RP2350-Zero's 3V3 does not have to run across the board; almost only 5 V crosses it |
| Knobs through a CD4051 (8 → 1) | The RP2350A has only 4 ADC channels (5 knobs + 3 CVs) |

Op-amp use: U1 = input buffers (A is R, B is L); U2 = L channel (A = WET output stage, B = MIX sum); U3 = R channel (same); U4A = VREF2.5 buffer (5 V divided by 100 k / 100 k, smoothed by 10 µF); U4B = unused (follower, + input tied to VREF).

### 2.2 Input and ADC (PCM1808)

| Part | Value | Reason |
|---|---|---|
| Series resistor at the jack | 1 kΩ | Short-circuit protection. With the sender's 1 kΩ, the loss into 100 kΩ is 0.17 dB |
| Pull to VREF2.5 | 100 kΩ | Unpatched inputs are silent (2.5 V). This also sets the input impedance |
| AC coupling | 1 µF X7R | The PCM1808 biases its input internally, so the DC is blocked. fc ≈ 1.7 Hz |
| Series resistor | 33 kΩ | Divides with the PCM1808's 60 kΩ (typ) input resistance: ×0.645. The standard 3 Vpp becomes 1.94 Vpp (−4.1 dBFS); the ADC clips only above 4.8 Vpp |
| Anti-aliasing | 100 pF C0G | With the 21 kΩ Thévenin resistance, fc ≈ 75 kHz |

- PCM1808: slave mode, I2S format. VCC = 5 V (through a ferrite bead), VDD = 3.3 V (XC6206).
- The 60 kΩ is a typical value, so the input gain varies by a few % between units. It only affects the WET level, so it is not calibrated.

### 2.3 DAC (PCM5102A) and WET output stage

- PCM5102A: SCK = GND (internal PLL from BCK), FMT = GND, DEMP = GND, FLT = 3.3 V (low-latency filter), XSMT = 3.3 V.
- Output: 470 Ω + 2.2 nF low-pass → inverting amplifier (×−1, 2.5 V centre) → 470 Ω → WET jack.
- A series 10 µF (C34 / C35) after the inverting amplifier's input resistor makes the stage AC coupled. With 100 kΩ, fc ≈ 0.16 Hz.
- The WET output swings 0.24–4.6 V. The firmware's soft clip limits it to about ±2.2 V (3.3).
- MIX is DRY + WET, so it distorts beyond 0–5 V when both are large. With the internal MIX, WET around 20–30% is the intended range.

**Board v1.0:** v1.0 has no C34 / C35, so its WET output stage is DC coupled. The VREF on the inverting amplifier's + input appears at twice its value, so with the DAC at 0 the WET centre is about 4.85 V (measured) and the upper half-wave is cut. v1.0.1 adds C34 / C35. On a v1.0 board, `CFG_E003 1` in `config.h` adds a DC offset to the DAC to bring the centre back to 2.5 V, but the WET swing is then limited to about 0.95 Vpp.

### 2.4 Clocks

- The PCM1808 needs SCKI (256 fs). PIO makes SCKI, BCK and LRCK by dividing one clock, and the ADC and DAC share BCK / LRCK.
- 150 MHz ÷ 12 = SCKI 12.5 MHz → fs = 48 828.125 Hz. Only the WET is digital, so fs does not need to be exactly 48 kHz; a value reachable by integer division was chosen.

### 2.5 Knobs and CVs

- The knobs (B10 kΩ) sit between the XC6206 3.3 V and GND. The CD4051 selects one and GP26 (ADC0) reads it. The CD4051 runs from the RP2350-Zero's 3V3, so its select inputs have the RP2350's logic levels.
- The knob supply (XC6206) and the ADC reference (the RP2350-Zero's 3.3 V) are different, so the reading is not fully ratiometric and can be off by a few %. The knobs do not need that precision.
- CV: jack → 10 kΩ / 15 kΩ divider, ×0.6 (5 V → 3.0 V) → 10 nF → ADC (GP27–29). The RP2350 ADC pins are not 5 V tolerant, so the divider is required. The upper 10 kΩ also limits the current. fc ≈ 2.6 kHz.

### 2.6 MODE button and LEDs

- MODE button: GP15 – 1 kΩ – switch – GND. Internal pull-up; pressed = low.
- LEDs: GP4 / GP5 – 1 kΩ – 3 mm LED – GND (about 1.3–1.5 mA). On or off only, no PWM (no PWM current in the ground).

### 2.7 Power

| Rail | Source | Loads |
|---|---|---|
| 5 V | AE bus | RP2350-Zero (through a Schottky diode), PCM1808 VCC (through a ferrite bead), op-amps, VREF, XC6206 |
| 3.3 V (audio) | XC6206P332MR | PCM1808 VDD, PCM5102A, knobs |
| 3.3 V (RP2350-Zero 3V3) | RP2350-Zero | CD4051 only |

- The RP2350-Zero's 5 V pin is fed through a Schottky diode (1N5819WS), so USB VBUS and the AE bus do not meet. 10 µF + 100 nF after the diode keep the RP2350's current pulses off the 5 V bus shared with the analog circuits.
- The audio 3.3 V load is about 30 mA; the XC6206 dissipates about 60 mW.

### 2.8 Pin assignment

| GP | Function |
|---|---|
| GP0 / GP1 / GP2 | CD4051 A / B / C |
| GP4 / GP5 | LED1 / LED2 |
| GP10 | SCKI (PCM1808) |
| GP11 | PCM1808 DOUT → RP2350 |
| GP12 | RP2350 → PCM5102A DIN |
| GP13 / GP14 | LRCK / BCK (shared by both ICs) |
| GP15 | MODE button |
| GP26 | Knobs (CD4051 COM) |
| GP27 / GP28 / GP29 | CV1 / CV2 / CV3 |

CD4051 channels: WET = CH2, PRE-DELAY = CH1, DECAY = CH0, TONE = CH3, CHARACTER = CH5 (CH4 / CH6 / CH7 are grounded). Each I2S line has a 33 Ω series resistor.

## 3. Firmware structure

```
ADC (L, R) ─ muted for 1 s after start-up
  └ PRE-DELAY (0–200 ms, separate L / R, linear interpolation, follows the knob over ~40 ms)
      └ input fade (150 ms S-curve after a mode change)
          └ mode (Plate / Hall / Room / Shimmer) ── stereo output
              └ 15 Hz DC block → sign inversion (the WET output stage inverts)
                  └ × WET gain (knob × per-mode correction) × output fade
                      └ limiter → soft clip → DAC
```

- Core 1 runs only the audio (PIO + DMA I2S, 32-frame blocks, 655 µs budget). Core 0 runs the knobs, CVs, MODE button, LEDs, mode memory and the USB console.
  - Reason: arduino-pico services USB on core 0. With the audio on core 0, every time the PC opened the serial port the USB interrupt stretched a block to about 1 ms, and audio dropped out at 80% load.
- The audio code runs from RAM and never calls newlib functions in flash (`powf`, `expf`, …). Both cores share the XIP cache; when console output evicted those functions, a block took about 1 ms and dropped out. Exponentials and logarithms use the polynomial approximations in `dsp.h`.
- The output is written two blocks ahead, so a single block may take up to 200% of the budget without a dropout. An average above 100% does drop out.

### 3.1 Knob and CV handling

- A CV of 0–5 V adds 0–1 to the knob position (`CV ÷ 5 V`). Below 0.05 V the input counts as unpatched (0). CV1 → WET, CV2 → DECAY, CV3 → TONE.
- Knob values pass a 0.3% dead band before they reach the audio side. Without it, ADC noise changed the parameters every 1 ms and the reverb-time calculation kept shifting.
- The audio side smooths them further over about 13 ms.

### 3.2 Mode switching and memory

- Switching: the WET fades out over an 80 ms S-curve → while it is silent, the old mode's delay memory is cleared a chunk per block → the new mode's output fades in over 50 ms and its input over 150 ms. The slow input fade keeps the early reflections from arriving all at once.
- One delay memory (60 000 floats = 240 KB) is shared by all modes and rebuilt on every switch.
- The selected mode is saved in flash. Core 1 stops during the write, so it is done during the silent part of the switch.

### 3.3 Level design

| Item | Value | Reason |
|---|---|---|
| WET knob | Linear | With a square-law curve the middle was −12 dB and the left half did nothing |
| Per-mode correction | Plate 0.77 / Hall 0.88 / Room 2.60 / Shimmer 1.15 × 0.77 | Set so that at WET 100% the WET RMS equals the input RMS, measured with an ADSR-gated phrase (input −18.9 dBFS RMS). Room alone is +6 dB: with equal RMS it still sounded weak in the MIX |
| Soft-clip ceiling | 0.75 FS (about ±2.2 V at the WET jack) | Stays within the 0.24–4.6 V swing of the WET output stage |
| Limiter | Threshold 0.8 × ceiling, 1 ms attack, 100 ms release | A safety net that normally does nothing |

- With that material at WET 100%: peaks 1.06–1.93 V, no soft clipping.
- With sustained sounds (not gated by an envelope) the reverb builds up and the limiter works all the time at WET 100%. Gated sounds were given priority.

## 4. Common knob meanings

| Knob | Meaning |
|---|---|
| DECAY | Reverb time (RT60), logarithmic. The range depends on the mode |
| TONE | Tilt: left = dark and thick, right = bright and thin. It moves the damping inside the reverb and the input band together; in the right half it also thins the input lows (high-pass 20 → 300 Hz) (Plate / Hall / Shimmer). In Room it is "distance" |
| CHARACTER | Depends on the mode |

## 5. Modes

The same information is in the header comment of each mode's source file. Sound decisions were made by listening; measurements were used as a check.

### 5.1 Plate (`mode_plate.cpp`)

- Dattorro's figure-8 tank (J. Dattorro, "Effect Design Part 1", JAES 1997), with the paper's lengths (for 29 761 Hz) scaled to fs.
- DECAY 0.4–15 s. The tank's decay gain is applied twice per half loop, so each application gets half of the half loop's attenuation.
- TONE: tank damping 0.6–18 kHz, input band 1.5–18 kHz.
- CHARACTER: left half = diffusion (0 = grainy, echo-like; centre = Dattorro's values), right half = tank modulation (±0–25 samples, 0.6–1.6 Hz).
- **Random wobble:** the tank modulation is a smooth random movement (`RandMod` in `dsp.h`), not a sine. Peaks and troughs are joined by half-cosines; each peak is 60–100% of full depth and each half-cycle ±30% long. The pitch swing (RMS rate of change of the delay) matches the sine's: 7.2 /s against 7.1 /s at 1.6 Hz.
  - Reason: with a sine the loop's notes always meet the wobble at a similar phase, which sounds as even as a sample library. With the random wobble each note's WET onset differs a little and sounds closer to live playing. The pitch swing has to match: a weaker wobble exposes the tank's own colour (lower mids).
- **Gentle saturation:** an asymmetric soft clip (drive 1.5, bias 0.5) on the input and in each half of the tank. The small-signal gain stays 1, so the reverb time does not change. A 5 Hz DC block in each half removes the DC from the asymmetry.
  - Reason: it sharpens the outline and its even harmonics add a little "box" resonance, which sets Plate apart from Hall. Higher drive made the tail muddy: the distortion adds up every loop and the intermodulation builds.
- Frequency-response roughness (99th percentile of the deviation from the 1/3-octave median): 5.8 dB. This is the reference for "ringing" in the other modes.

### 5.2 Hall (`mode_fdn.cpp`)

- An 8-line FDN (Householder matrix). Lines of 66.1–146.7 ms (about 0.83 s in total), rounded to prime sample counts. One allpass in each line (19.7–44.9 ms, g 0.6). Input diffusion: 4 allpasses.
  - Reason: with short lines the resonance peaks were sparse and single notes (C5, for example) rang. Longer lines and long in-line allpasses give a response as smooth as Plate's.
- DECAY 0.8–20 s. TONE is the tilt. CHARACTER = size (line lengths ×0.5–×1.0; turning it bends the pitch slightly).
- **Bass ratio:** below 250 Hz each line decays 1.3 times longer (real concert halls are 1.1–1.3). Measured (DECAY 0.5): 150–300 Hz reverb time 4.33 → 4.94 s; 1–2 kHz unchanged.
  - Effect: the extra bass is in the tail, not in the attack, so it sounds warm and enveloping rather than boomy.
- **Random wobble per line:** each of the 8 lines wobbles on its own (0.29–0.47 Hz, ±16 samples). The pitch swing matches a sine's, but there is no regular beating, and the reverb's spectral balance improves (with sines, some parts of the mids were too crowded).

### 5.3 Room (`mode_room.cpp`)

- Early reflections + Plate's tank scaled to 0.45 (half loop about 0.16 s). Input diffusion as in Plate. Tank modulation ±3 samples (0.9 Hz).
  - Reason: a shortened version of the Hall FDN kept ringing on single notes (C5) whatever the delays. Plate's tank never did, so it was scaled down.
- Early reflections: 8 taps, L 4.3–34.9 ms, R 5.1–36.2 ms. Gains rise and fall (0.30 → 0.52 → 0.30, alternating signs). Their sum is blurred by 2 allpasses and a 7 kHz low-pass.
  - Reason: a strong first reflection sounds like a delayed copy of the DRY, and the mode sounded "dry" in the MIX (about 20% of the energy in the first 10 ms).
- DECAY 0.2–4 s. CHARACTER: left = mostly tail, right = strong early reflections.
- Aim: "wistful, nostalgic, a piano in a school music room". To work on synths too, it uses the room's contribution to that sound, not the instrument's (string beating, detuning).
- **Mid dip:** −2.5 dB around 460 Hz (a 300–700 Hz band) on the tail output.
  - Reason: low mids piled up in the tail note after note and sounded crowded. The measured reverb time was flat across bands, so no band was ringing. Making the mids decay faster inside the loop was tried, but the tail lost its core over time and the WET sounded closer to the DRY. A fixed dip calms the whole tone.
- **Wood warmth:** +2 dB around 200 Hz (120–350 Hz) on the whole WET, then −1.3 dB overall to keep the level. The band just above is dipped, so the contrast between the bass and the boxy band grows: the bass goes up, yet the outline becomes cleaner.
- **TONE = distance:** in Room, TONE changes distance, not brightness. The filters inside the tank stay at their TONE-fully-right values (damping 18 kHz, input band 18 kHz, input high-pass 300 Hz). With distance f = 1 − TONE:
  - early-reflection gain 1 − 0.8f; early-reflection low-pass 7 kHz × (4/7)^(2f) (4 kHz at the centre, 2.3 kHz fully left); one more allpass blended in over f = 0–0.5; level correction +2.0 dB × f (within ±0.5 dB over the whole knob).
  - Reason: a low-pass inside the tank acts on every loop, so the highs vanish more with each pass and the reverb collects in the mids. Darkening only the early reflections, which do not recirculate, leaves the tail's balance alone and gives the natural cue that distant sounds lose their highs first.

### 5.4 Shimmer (`mode_shimmer.cpp`)

- Hall's network (8-line FDN, bass ratio, per-line random wobble, input diffusion), with its output shifted up an octave and a fifth and fed back into its input. Each pass climbs higher and dies away in the TONE damping.
- Pitch shifting: two taps sweep a window (40 ms L, 43 ms R) at 2× speed (1.5× for the fifth) and are joined by a sin² crossfade. The octave and the fifth read the same delay, so no extra memory is needed. The windows differ between L and R so their seams do not line up.
- Before the shifter: two 7 kHz low-passes (reading at 2× speed folds anything above fs/4) and a 250 Hz high-pass (keeps climbing bass from getting muddy). The L output feeds the R input and vice versa, for width. The fed-back signal is soft-clipped.
- **35% fifth:** the shifted power is split 65% octave and 35% fifth (equal-power).
  - Reason: with octaves alone, every pass lands only on 2, 4, 8 × the original, and that sparse series sounded thin, slightly metallic and "plug-in like". The fifth fills in 1.5, 3, 4.5 × (3 × is in the natural harmonic series), giving a thicker sparkle. The ratio was chosen by listening to 0 / 25 / 50 / 75 / 100% and then 30 / 40%.
  - A random detune of the shifted voices (about ±8 cents) was also tried. It was inaudible under the network's own wobble (±16 samples), so it is not used.
- DECAY 0.8–20 s. TONE is the tilt (it also sets how high the shifted voices climb). CHARACTER = shimmer amount (0 is almost Hall; fully right is a feedback gain of 0.55). Fully right is strong but does not fall apart. It stays stable with DECAY and CHARACTER both fully right (checked 2026-11-26: the impulse response falls about 57 dB in 21 s; the reverb time is 25.1 s against a 19.9 s target, a little longer because the climbing voices keep adding energy).

## 6. Verification (board v1.0.1, 2026-11-23 to 26)

### 6.1 Processing load (average, against the 655 µs block)

| Plate | Hall | Room | Shimmer |
|---|---|---|---|
| 37% | 64% | 47% | 85% |

- The worst single block is within a few % of the average in every mode. No dropouts, also while the USB console is in use.
- Delay memory used: Plate 37 k, Hall 55 k, Room 21 k, Shimmer 59 k (of 60 k floats).

### 6.2 Reverb time and response

- Reverb times were checked against the DECAY target with impulse responses (`imp` / `tail`, from the −5 to −25 dB slope). Example: Plate at DECAY 0.5 measures 2.33 s against a 2.45 s target.
- Frequency-response roughness (99th percentile): Plate 5.8 dB, Hall 7.5 dB, Room 8.0 dB. At these values no single note rang.
- Energy in the first 10 ms (relative to the whole response): Hall −19.6 dB, Room −17.1 dB.

### 6.3 Levels

- At WET 100% with an ADSR-gated phrase, the WET output peaks at 1.06–1.93 V, with no soft clipping.
- The WET RMS is about the same in every mode (Room is +6 dB on purpose).

## 7. Known limitations

- Listening tests used mainly ADSR-gated synth phrases. Sustained sounds make the WET louder, and at WET 100% the limiter works all the time.
- MIX is DRY + WET, so a large WET can exceed 0–5 V and distort.
- The PCM1808 input resistance is a typical value, so the WET level varies by a few % between units.
