// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shota Azuma
// State shared between core1 (controls, console) and core0 (audio). Single writer per field.
#pragma once
#include <stdint.h>
#include "config.h"

struct Shared {
    // core1 -> core0: combined knob + CV positions, 0..1
    volatile float pos[K_COUNT];
    volatile int mode_req;          // requested mode (core0 follows with a fade)
    volatile float dbg_mod;         // debug: modulation depth scale 0..1 (console `dbg mod`)
    volatile int dbg_lim;           // debug: 1 = limiter on (console `dbg lim`)
    volatile float tone_hz, tone_ms; // test: `tone` injects a sine burst instead of the impulse
    volatile int imp_req;           // 1 = inject an impulse and record the tail envelope

    // mode memory (flash, EEPROM emulation). The write pauses core0 for ~50 ms, so it is done while the
    // wet output is already silent inside a mode switch: core0 sets save_ready once the whole TX ring
    // holds silence, core1 commits and sets save_done.
    volatile int mode_saved;
    volatile int save_mode, save_ready, save_done;
    volatile uint32_t saves;

    // core0 -> core1
    volatile int mode_cur;
    volatile uint32_t blocks, underruns, resyncs, clips;
    volatile float lim_min;         // lowest limiter gain since `load reset`
    volatile uint32_t lim_blocks;   // blocks in which the limiter reduced the gain
    volatile float wet_peak;
    volatile float in_ss, rev_ss;   // sum of squares since `load reset`: live input (L+R) and raw mode output
    volatile uint32_t ss_n;        // highest |wet| at the DAC (after gain, before the limiter), FS
    volatile uint32_t load_us_max, load_us_sum, load_n;   // per-block processing time
    volatile float rt60_now;        // current mode's RT60 for the current DECAY
    volatile int arena_used;        // floats
    volatile int arena_fail;
};
extern Shared g_sh;

// impulse-response envelope recorder (core0 writes, core1 prints)
#define ENV_BINS 2048
#define ENV_BIN_FRAMES 512
extern float g_env[ENV_BINS];
// raw impulse response (mode output L, before WET gain), int16 with IR_SCALE, first IR_N samples
#define IR_N 40000
#define IR_SCALE 131068.0f      // 32767 / 0.25
extern int16_t g_ir[IR_N];
extern volatile int g_ir_n;
extern volatile int g_env_n;        // bins filled
extern volatile int g_env_done;
