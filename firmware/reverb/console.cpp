// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shota Azuma
// USB serial console (core1). One command per line; every reply ends with "ok".
#include "console.h"

#include <Arduino.h>
#include <math.h>
#include "config.h"
#include "control.h"
#include "mode_store.h"
#include "shared.h"

// wait without blocking the flash save (it must run on core1 while the engine holds the wet silent)
static void wait_ms(uint32_t ms) {
    uint32_t t0 = millis();
    while (millis() - t0 < ms) { mode_store_poll(); delay(1); }
}

static const char *MODE_NAMES[MODE_COUNT] = {"plate", "hall", "room", "shimmer"};
static const char *KNOB_NAMES[K_COUNT] = {"wet", "pre", "decay", "tone", "char"};

static void cmd_info() {
    Serial.printf("%s  mode=%d(%s) req=%d saved=%d (saves %lu)  rt60=%.2fs  arena=%d/%d%s\n", FW_VERSION,
                  g_sh.mode_cur, MODE_NAMES[g_sh.mode_cur], g_sh.mode_req, g_sh.mode_saved,
                  (unsigned long)g_sh.saves, g_sh.rt60_now, g_sh.arena_used, CFG_ARENA_FLOATS,
                  g_sh.arena_fail ? " FAIL" : "");
    for (int k = 0; k < K_COUNT; k++) {
        Serial.printf("  %-5s knob=%.3f pos=%.3f%s\n", KNOB_NAMES[k], control_knob(k), g_sh.pos[k],
                      control_override(k) >= 0 ? " (override)" : "");
    }
    Serial.printf("  cv1=%.3fV cv2=%.3fV cv3=%.3fV\n", control_cv_volts(0), control_cv_volts(1), control_cv_volts(2));
}

static void cmd_load(bool reset) {
    uint32_t n = g_sh.load_n;
    float avg = n ? (float)g_sh.load_us_sum / n : 0;
    float budget = CFG_BLOCK * 1e6f / (150e6f / (CFG_FS_DIV * 256.0f));
    Serial.printf("load avg=%.1fus (%.1f%%) max=%luus (%.1f%%) budget=%.0fus blocks=%lu underruns=%lu resyncs=%lu clips=%lu\n",
                  avg, 100 * avg / budget, (unsigned long)g_sh.load_us_max, 100.0f * g_sh.load_us_max / budget, budget,
                  (unsigned long)g_sh.blocks, (unsigned long)g_sh.underruns, (unsigned long)g_sh.resyncs,
                  (unsigned long)g_sh.clips);
    Serial.printf("limiter min_gain=%.3f (%.1fdB) active_blocks=%lu  wet_peak=%.3fFS (%.1fdB re limit, ~%.2fV)\n",
                  g_sh.lim_min, 20 * log10f(g_sh.lim_min), (unsigned long)g_sh.lim_blocks, g_sh.wet_peak,
                  20 * log10f(g_sh.wet_peak / CFG_WET_LIMIT + 1e-9f), g_sh.wet_peak * 2.9f);
    if (g_sh.ss_n) {
        float ir = sqrtf(g_sh.in_ss / g_sh.ss_n), rr = sqrtf(g_sh.rev_ss / g_sh.ss_n);
        Serial.printf("levels in_rms=%.4fFS (%.1fdBFS) rev_rms=%.4fFS (%.1fdBFS) rev/in=%.2fdB\n", ir,
                      20 * log10f(ir + 1e-9f), rr, 20 * log10f(rr + 1e-9f), 20 * log10f((rr + 1e-9f) / (ir + 1e-9f)));
    }
    if (reset) {
        g_sh.load_us_max = 0; g_sh.load_us_sum = 0; g_sh.load_n = 0;
        g_sh.clips = 0; g_sh.lim_min = 1.0f; g_sh.lim_blocks = 0; g_sh.wet_peak = 0;
        g_sh.in_ss = 0; g_sh.rev_ss = 0; g_sh.ss_n = 0;
    }
}

// impulse response: decay curve and RT60 from the -5 .. -25 dB slope (T20 x 3)
static void cmd_tail() {
    int n = g_env_n;
    if (n < 4) { Serial.println("ERR no data (imp first)"); return; }
    float pk = 0; int ipk = 0;
    for (int i = 0; i < n; i++) if (g_env[i] > pk) { pk = g_env[i]; ipk = i; }
    if (pk <= 0) { Serial.println("ERR silent"); return; }
    const float bin_s = ENV_BIN_FRAMES / (150e6f / (CFG_FS_DIV * 256.0f));
    // Schroeder backward integration
    static float sch[ENV_BINS];
    float acc = 0;
    for (int i = n - 1; i >= 0; i--) { acc += g_env[i]; sch[i] = acc; }
    float t5 = -1, t25 = -1;
    for (int i = ipk; i < n; i++) {
        float d = 10 * log10f(sch[i] / sch[ipk]);
        if (t5 < 0 && d <= -5) t5 = i * bin_s;
        if (t25 < 0 && d <= -25) { t25 = i * bin_s; break; }
    }
    Serial.printf("tail bins=%d (%.2fs) peak@%.3fs %.1fdBFS  done=%d\n", n, n * bin_s, ipk * bin_s,
                  10 * log10f(pk), (int)g_env_done);
    if (t5 >= 0 && t25 > t5) Serial.printf("rt60_est=%.2fs (T20 x3)  target=%.2fs\n", 3 * (t25 - t5), g_sh.rt60_now);
    else Serial.println("rt60_est=n/a (record longer)");
    Serial.print("env_dB:");
    int step = n / 40 > 0 ? n / 40 : 1;
    for (int i = 0; i < n; i += step) Serial.printf(" %.0f", 10 * log10f(g_env[i] / pk + 1e-12f));
    Serial.println();
}

static void handle(char *line) {
    char *argv[6]; int argc = 0;
    for (char *t = strtok(line, " \t"); t && argc < 6; t = strtok(nullptr, " \t")) argv[argc++] = t;
    if (!argc) return;
    String c = argv[0];
    if (c == "help") {
        Serial.println("info | load [reset] | mode <0-3|plate|hall|room|shimmer> | next");
        Serial.println("set <wet|pre|decay|tone|char> <0..1> | default | imp | tail");
        Serial.println("dbg <mod 0..1|lim 0/1>");
    } else if (c == "info") cmd_info();
    else if (c == "load") cmd_load(argc > 1);
    else if (c == "next") { control_next_mode(); cmd_info(); }
    else if (c == "mode" && argc > 1) {
        int m = -1;
        for (int i = 0; i < MODE_COUNT; i++) if (!strcmp(argv[1], MODE_NAMES[i])) m = i;
        if (m < 0) m = atoi(argv[1]);
        if (m < 0 || m >= MODE_COUNT) { Serial.println("ERR mode"); }
        else { g_sh.mode_req = m; wait_ms(400); cmd_info(); }
    } else if (c == "set" && argc > 2) {
        int k = -1;
        for (int i = 0; i < K_COUNT; i++) if (!strcmp(argv[1], KNOB_NAMES[i])) k = i;
        if (k < 0) Serial.println("ERR knob");
        else { control_set_override(k, atof(argv[2])); wait_ms(30); cmd_info(); }
    } else if (c == "default") { control_clear_overrides(); wait_ms(30); cmd_info(); }
    else if (c == "cycle" && argc > 2) {   // switch modes internally (no USB traffic), then report underruns
        int cnt = atoi(argv[1]), ms = atoi(argv[2]);
        uint32_t u0 = g_sh.underruns, r0 = g_sh.resyncs;
        g_sh.load_us_max = 0;
        for (int i = 0; i < cnt; i++) { control_next_mode(); wait_ms(ms); }
        Serial.printf("cycle %d x %dms: underruns +%lu resyncs +%lu max=%luus\n", cnt, ms,
                      (unsigned long)(g_sh.underruns - u0), (unsigned long)(g_sh.resyncs - r0),
                      (unsigned long)g_sh.load_us_max);
    }
    else if (c == "tone" && argc > 2) {
        g_sh.tone_hz = atof(argv[1]); g_sh.tone_ms = atof(argv[2]);
        g_env_n = 0; g_env_done = 0; g_sh.imp_req = 1; Serial.println("tone armed");
    }
    else if (c == "imp") { g_sh.tone_hz = 0; g_env_n = 0; g_env_done = 0; g_sh.imp_req = 1; Serial.println("impulse armed"); }
    else if (c == "dbg" && argc > 2) {
        if (!strcmp(argv[1], "mod")) { float v = atof(argv[2]); g_sh.dbg_mod = v < 0 ? 0 : (v > 1 ? 1 : v); }
        else if (!strcmp(argv[1], "lim")) g_sh.dbg_lim = atoi(argv[2]) ? 1 : 0;
        Serial.printf("dbg mod=%.2f lim=%d\n", g_sh.dbg_mod, g_sh.dbg_lim);
    }
    else if (c == "tail") cmd_tail();
    else if (c == "ir") {   // raw impulse response captured by `imp` (int16, scale 32767 = 0.25)
        int n = g_ir_n;
        Serial.printf("ir begin n=%d scale=%.1f\n", n, IR_SCALE);
        for (int i = 0; i < n; i++) { Serial.printf("%d\n", g_ir[i]); if ((i & 255) == 255) delay(1); }
        Serial.println("ir end");
    }
    else Serial.println("ERR unknown (help)");
    Serial.println("ok");
}

void console_setup() { Serial.begin(115200); }

void console_loop() {
    static char buf[96]; static int n = 0;
    while (Serial.available()) {
        int ch = Serial.read();
        if (ch == '\r' || ch == '\n') {
            if (n) { buf[n] = 0; handle(buf); n = 0; }
        } else if (n < (int)sizeof(buf) - 1) buf[n++] = (char)ch;
    }
}
