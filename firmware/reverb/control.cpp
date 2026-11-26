// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shota Azuma
// core1: knobs (CD4051), CV inputs, MODE button, LEDs. Publishes g_sh.pos[] and g_sh.mode_req.
#include "control.h"

#include <Arduino.h>
#include <initializer_list>
#include <math.h>
#include "config.h"
#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "shared.h"

static float s_knob[K_COUNT];      // smoothed 0..1
static float s_cv[3];              // smoothed volts at the jack
static float s_override[K_COUNT];  // < 0 = use the knob (console `set`)
// Published positions only move when the new value differs by more than HYST. Without this, ADC noise
// changed the parameters every 1 ms; the audio core picks them up every block (1526 Hz), and the beat
// (~526 Hz) modulated the reverb -- a loud "foon" around C5, worst in HALL (v0.5 investigation).
static float s_pub[K_COUNT];
#define HYST 0.003f
static bool s_first = true;

// button
static bool s_btn_last = true;     // pull-up: idle high
static uint32_t s_btn_t = 0;

static uint16_t adc_avg(uint input, int n) {
    adc_select_input(input);
    (void)adc_read();
    uint32_t s = 0;
    for (int i = 0; i < n; i++) s += adc_read();
    return (uint16_t)(s / n);
}

void control_setup() {
    adc_init();
    for (int p = 26; p <= 29; p++) adc_gpio_init(p);
    for (int b = 0; b < 3; b++) {
        gpio_init(PIN_MUX_A + b);
        gpio_put(PIN_MUX_A + b, 0);
        gpio_set_dir(PIN_MUX_A + b, true);
    }
    for (int p : {PIN_LED1, PIN_LED2}) {
        gpio_init(p);
        gpio_put(p, 0);
        gpio_set_dir(p, true);
    }
    gpio_init(PIN_MODE);
    gpio_set_dir(PIN_MODE, false);
    gpio_pull_up(PIN_MODE);
    for (int k = 0; k < K_COUNT; k++) s_override[k] = -1.0f;
}

void control_set_override(int k, float v) { if (k >= 0 && k < K_COUNT) s_override[k] = v; }
void control_clear_overrides() { for (int k = 0; k < K_COUNT; k++) s_override[k] = -1.0f; }
float control_knob(int k) { return s_knob[k]; }
float control_cv_volts(int i) { return s_cv[i]; }
float control_override(int k) { return s_override[k]; }

void control_next_mode() { g_sh.mode_req = (g_sh.mode_req + 1) % MODE_COUNT; }

// called from loop1 about every 1 ms
void control_loop() {
    // knobs
    for (int k = 0; k < K_COUNT; k++) {
        int ch = KNOB_MUX_CH[k];
        for (int b = 0; b < 3; b++) gpio_put(PIN_MUX_A + b, (ch >> b) & 1);
        busy_wait_us(40);
        float v = ((float)adc_avg(0, 8) - POT_RAW_MIN) / (POT_RAW_MAX - POT_RAW_MIN);
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        s_knob[k] = s_first ? v : s_knob[k] + (v - s_knob[k]) * 0.2f;
    }
    // CV
    for (int i = 0; i < 3; i++) {
        float v = adc_avg(1 + i, 8) * CV_VOLTS_PER_COUNT;
        s_cv[i] = s_first ? v : s_cv[i] + (v - s_cv[i]) * 0.3f;
    }
    s_first = false;
    // publish knob + CV
    float pos[K_COUNT];
    for (int k = 0; k < K_COUNT; k++) pos[k] = s_knob[k];
    for (int i = 0; i < 3; i++) {
        float c = s_cv[i] < 0.05f ? 0.0f : s_cv[i] / CV_FULL_SCALE_V;   // ignore the unpatched ~0.02 V
        pos[CV_TARGET[i]] += c;
    }
    for (int k = 0; k < K_COUNT; k++) {
        float p = s_override[k] >= 0 ? s_override[k] : pos[k];
        if (p < 0) p = 0;
        if (p > 1) p = 1;
        // ends snap exactly to 0 / 1 so a knob turned fully left / right reaches the end value
        if (fabsf(p - s_pub[k]) > HYST || (p == 0.0f && s_pub[k] != 0.0f) || (p == 1.0f && s_pub[k] != 1.0f) ||
            s_override[k] >= 0) {
            s_pub[k] = p;
            g_sh.pos[k] = p;
        }
    }

    // MODE button: falling edge, 30 ms debounce
    bool b = gpio_get(PIN_MODE);
    uint32_t now = millis();
    if (b != s_btn_last && now - s_btn_t > 30) {
        s_btn_t = now;
        s_btn_last = b;
        if (!b) control_next_mode();
    }
    // LEDs: binary mode number, LED1 = MSB (on/off only, no PWM)
    int m = g_sh.mode_req;
    gpio_put(PIN_LED1, (m >> 1) & 1);
    gpio_put(PIN_LED2, m & 1);
}
