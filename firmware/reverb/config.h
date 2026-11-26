// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shota Azuma
// REVERB multi-reverb firmware: build configuration (spec: spec/reverb_spec.md)
#pragma once

#define FW_VERSION "reverb 0.22a"

// ---- board revision ---------------------------------------------------------------------------
// E003 (v1.0 boards): the WET stage is DC coupled, WET = 2*VREF - Vdac. A DAC DC of +0.836 FS puts the
// WET centre at VREF; the clean swing is then only about +-0.16 FS (+-0.46 V at the jack).
// Set to 0 for v1.0.1+ boards (series 10 uF C34/C35 added).
#define CFG_E003 0   // 2026-11-23: v1.0.1 board (C34/C35 fitted)
#if CFG_E003
#define CFG_DAC_BIAS   0.836f
#define CFG_WET_LIMIT  0.16f   // soft-clip ceiling of the wet signal at the DAC, FS
#else
#define CFG_DAC_BIAS   0.0f
#define CFG_WET_LIMIT  0.75f   // about +-2.2 V at the jack (WET swings 0.24 .. 4.6 V; was 0.50 = +-1.45 V)
#endif
#define CFG_WET_GAIN   1.0f    // global trim (0.35 on v1.0; 1.0 on v1.0.1, 2026-11-23)
#define CFG_WET_TAPER  1.0f    // WET knob gain = pos^TAPER. 1.0 = linear (trial 2026-11-23); 2.32 put 50 % at 0.2 (-14 dB)
// Per-mode output normalisation: at WET 100 % the WET jack has the same RMS as the input jack.
// Measured 2026-11-23 with the main test material (ADSR-gated loop, input -18.9 dBFS RMS):
// rev/in RMS plate +0.9, hall -0.3, room -3.7, spring -2.6 dB -> norm = 0.853 * 10^(-rev/in / 20)
// v0.22a: slot 3 is SHIMMER; it keeps the SPRING norm and matches it with its own SHIM_TRIM (mode_shimmer.cpp)
// (0.853 = input full scale 2.475 V peak / DAC full scale 2.9 V peak). Sustained (un-gated) material
// builds up more and leans on the limiter.
// v0.7b: ROOM +6 dB by ear (needed ~50 % WET to sound right while PLATE / HALL sit lower)
static const float MODE_WET_NORM[4] = {0.77f, 0.88f, 2.60f, 1.15f};

// ---- audio ------------------------------------------------------------------------------------
#define CFG_FS_DIV        12        // 150 MHz / 12 = SCKI 12.5 MHz = 256 fs
#define CFG_BLOCK         32        // frames per processing block
#define CFG_RING_FRAMES   256       // DMA ring (power of 2)
#define CFG_OUT_AHEAD_BLK 3         // write position = completed block + D + 3 blocks
#define CFG_STARTUP_MUTE_MS 1000    // PCM1808 settles ~0.8 s after the clocks start
#define CFG_BIAS_RAMP_MS  800       // ramp the E003 DAC bias at start-up (no step on WET / MIX)
#define CFG_FADE_OUT_MS    80       // mode change: wet output S-curve fade out
#define CFG_FADE_IN_OUT_MS 50       // ... then the new mode's output fades in
#define CFG_FADE_IN_IN_MS  150      // ... while its input fades in (no burst of early reflections)
#define CFG_LIM_ATTACK_MS  1.0f     // wet limiter (threshold 0.8 x CFG_WET_LIMIT, before the soft clip)
#define CFG_LIM_RELEASE_MS 100.0f
#define CFG_PREDELAY_MAX_S 0.200f
#define CFG_ARENA_FLOATS  60000     // shared delay memory for the active reverb mode (240 KB)

// ---- pins (netlist 2026-11-12) ----------------------------------------------------------------
#define PIN_MUX_A 0      // GP0/1/2 = CD4051 A/B/C
#define PIN_LED1  4      // MSB of the mode number
#define PIN_LED2  5      // LSB
#define PIN_MODE  15     // MODE button to GND (R38 1k), internal pull-up
#define PIN_SCKI  10
#define PIN_DOUT  11
#define PIN_DIN   12
#define PIN_LRCK  13
#define PIN_BCK   14

// knobs: MUX channel of each knob, top to bottom (RV1..RV5)
enum { K_WET, K_PRE, K_DECAY, K_TONE, K_CHAR, K_COUNT };
static const int KNOB_MUX_CH[K_COUNT] = {2, 1, 0, 3, 5};
#define POT_RAW_MIN 8.0f
#define POT_RAW_MAX 4040.0f      // XC6206 3.3 V reads ~4048 on the RP2350 ADC

// CV inputs: CV1 -> WET, CV2 -> DECAY, CV3 -> TONE (ADC inputs 1..3 = GP27..29)
static const int CV_TARGET[3] = {K_WET, K_DECAY, K_TONE};
#define CV_VOLTS_PER_COUNT (3.3f / 4095.0f / 0.6f)  // 10k / 15k divider
#define CV_FULL_SCALE_V 5.0f                         // 0..5 V adds 0..1 to the knob position

enum { MODE_PLATE, MODE_HALL, MODE_ROOM, MODE_SHIMMER, MODE_COUNT };   // v0.22a: SHIMMER replaced SPRING
