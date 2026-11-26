// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shota Azuma
// REVERB multi-reverb (Plate / Hall / Room / Shimmer) for the AE Modular REVERB board.
//   core1: audio only (engine_run never returns)  — PIO/DMA I2S, 32-frame blocks, fs = 48828.125 Hz
//   core0: knobs / CV / MODE button / LEDs (control.*), mode memory and the USB console (console.*)
// v0.4: audio moved from core0 to core1. arduino-pico services USB on core0, and every time the PC opened
// the serial port the USB interrupt stretched audio blocks to ~1 ms (underruns at 80 % load).
// Spec: spec/reverb_spec.md.  E003 workaround: config.h CFG_E003.
#include <Arduino.h>
#include "config.h"
#include "console.h"
#include "control.h"
#include "engine.h"
#include "mode_store.h"
#include "shared.h"

// core1 runs only the audio loop: no SysTick interrupt there
bool core1_disable_systick = true;

static volatile bool s_ready = false;

void setup() {
    int m = mode_store_load();
    g_sh.mode_req = m >= 0 ? m : MODE_PLATE;
    g_sh.mode_saved = m >= 0 ? m : MODE_PLATE;   // a blank store starts in PLATE and is written on the first switch
    g_sh.pos[K_WET] = 0.5f;
    g_sh.pos[K_PRE] = 0.0f;
    g_sh.pos[K_DECAY] = 0.5f;
    g_sh.pos[K_TONE] = 0.5f;
    g_sh.pos[K_CHAR] = 0.5f;
    g_sh.dbg_mod = 1.0f;
    g_sh.dbg_lim = 1;
    control_setup();
    console_setup();
    s_ready = true;
}

void loop() {
    static uint32_t t = 0;
    uint32_t now = micros();
    if (now - t >= 1000) {
        t = now;
        control_loop();
    }
    mode_store_poll();
    console_loop();
}

void setup1() {
    while (!s_ready) {}
}

void loop1() { engine_run(); }
