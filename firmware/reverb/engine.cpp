// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shota Azuma
// core0: block loop, pre-delay, mode switching, wet gain, DAC formatting (E003 bias)
#include "engine.h"

#include <Arduino.h>
#include "audio_io.h"
#include "config.h"
#include "dsp.h"
#include "modes.h"
#include "shared.h"

Shared g_sh;
float g_env[ENV_BINS];
int16_t g_ir[IR_N];
volatile int g_ir_n = 0;
volatile int g_env_n = 0;
volatile int g_env_done = 0;

static float s_arena_buf[CFG_ARENA_FLOATS];
static Arena s_arena;

#define PD_LEN ((int)(CFG_PREDELAY_MAX_S * 49000.0f) + 8)
static float s_pd_buf[2][PD_LEN];
static Delay s_pdL, s_pdR;

static ReverbMode *s_modes[MODE_COUNT];
static ReverbMode *s_cur = nullptr;
static int s_cur_idx = 0;
static float s_fs = 48828.125f;

// smoothed controls
static float s_wet = 0, s_pre = 0;
static ModeParams s_mp = {0.5f, 0.5f, 0.5f};
static uint32_t s_frames = 0;     // since start
static OnePole s_dcL, s_dcR;
static float s_dc_a;

// mode switching: RUN -> FADE_OUT (output S-curve to 0) -> CLEAR (zero the arena a chunk per block,
// output silent) -> init -> FADE_IN (output S-curve up, reverb input S-curve up over a longer time)
enum SwState { SW_RUN, SW_FADE_OUT, SW_CLEAR, SW_SAVE, SW_FADE_IN };
static int s_save_blocks = 0;
static int s_nostat = 0;          // blocks to leave out of the load / underrun statistics (flash save pause)
#define SAVE_SILENT_BLOCKS (CFG_RING_FRAMES / CFG_BLOCK + 2)   // whole TX ring silent before core0 pauses
#define SAVE_TIMEOUT_BLOCKS 400                                  // ~0.26 s: give up if core1 does not answer
static SwState s_sw = SW_FADE_IN;
static float s_out_pos = 0;       // output fade position 0..1 (gain = S-curve of it)
static float s_in_pos = 0;        // reverb-input fade position 0..1
static float s_out_g0 = 0, s_out_g1 = 0;   // output fade gain at block start / end (per-sample ramp)
static int s_dirty = 0;           // arena floats that may be non-zero
static int s_clear_at = 0;
#define CLEAR_CHUNK 6144          // floats zeroed per block (~24 KB)

static inline float scurve(float p) { return fast_scurve(p); }

// limiter in front of the soft clip (stereo-linked peak, fast attack / slow release)
static float s_lim_gain = 1.0f;
static float s_lim_att, s_lim_rel;
#define LIM_THRESH (0.8f * CFG_WET_LIMIT)

// impulse test
static int s_env_active = 0, s_env_bin = 0, s_env_k = 0;
static float s_env_acc = 0;
static int s_tone_left = 0;        // test tone burst: samples left
static Lfo s_tone;

// the arena must already be zero (start-up: static storage; switching: SW_CLEAR)
static void init_mode(int idx) {
    s_arena.reset();
    s_cur_idx = idx;
    s_cur = s_modes[idx];
    s_cur->init(s_arena, s_fs);
    if (s_arena.used > s_dirty) s_dirty = s_arena.used;
    g_sh.mode_cur = idx;
    g_sh.arena_used = s_arena.used;
    g_sh.arena_fail = s_arena.fail;
}

static inline float smooth(float y, float x, float k) { return y + (x - y) * k; }

RAMFN static void process_block(const float *inL0, const float *inR0, float *outL, float *outR) {
    const int n = CFG_BLOCK;
    float inL[CFG_BLOCK], inR[CFG_BLOCK];

    // ---- controls (once per block) ----
    const float k = 0.05f;   // ~ 20 blocks = 13 ms
    float pw = g_sh.pos[K_WET];
    // WET taper: 50 % -> 0.2 (-14 dB, internal MIX), 100 % -> 1.0 (external mixing). 0.2 = 0.5^2.32
    float wt = pw > 0.0005f ? fast_exp2(CFG_WET_TAPER * fast_log2(pw)) : 0.0f;
    s_wet = smooth(s_wet, wt, k);
    float pp = g_sh.pos[K_PRE];
    float pre_target = pp * pp * CFG_PREDELAY_MAX_S * s_fs;
    s_mp.decay = smooth(s_mp.decay, g_sh.pos[K_DECAY], k);
    s_mp.tone = smooth(s_mp.tone, g_sh.pos[K_TONE], k);
    s_mp.character = smooth(s_mp.character, g_sh.pos[K_CHAR], k);

    // ---- mode change state machine ----
    int req = g_sh.mode_req;
    if (req != s_cur_idx && (s_sw == SW_RUN || s_sw == SW_FADE_IN)) s_sw = SW_FADE_OUT;
    // impulse test: first empty the current mode (otherwise the tail of the music that was playing is
    // recorded too -- the 520 Hz "resonance" of the v0.5 investigation was a C5 in the test loop)
    if (g_sh.imp_req == 1 && s_sw == SW_RUN) { g_sh.imp_req = 2; s_sw = SW_FADE_OUT; }
    s_out_g0 = scurve(s_out_pos);
    switch (s_sw) {
    case SW_FADE_OUT:
        s_out_pos -= (float)n / (CFG_FADE_OUT_MS * 0.001f * s_fs);
        if (s_out_pos <= 0) { s_out_pos = 0; s_sw = SW_CLEAR; s_clear_at = 0; }
        break;
    case SW_CLEAR: {
        int len = s_dirty - s_clear_at;
        if (len > CLEAR_CHUNK) len = CLEAR_CHUNK;
        if (len > 0) memset(s_arena_buf + s_clear_at, 0, (size_t)len * sizeof(float));
        s_clear_at += len;
        if (s_clear_at >= s_dirty) {
            s_dirty = 0;
            if (g_sh.mode_req != g_sh.mode_saved) {
                g_sh.save_mode = g_sh.mode_req;
                g_sh.save_done = 0;
                s_save_blocks = 0;
                s_sw = SW_SAVE;
            } else {
                init_mode(g_sh.mode_req);
                if (g_sh.imp_req) {   // also empty the pre-delay before an impulse test
                    memset(s_pd_buf, 0, sizeof(s_pd_buf));
                    s_pre = 0;
                }
                s_in_pos = 0;
                s_sw = SW_FADE_IN;
            }
        }
        for (int i = 0; i < n; i++) { outL[i] = 0; outR[i] = 0; }
        s_out_g1 = 0;
        return;   // the old mode's memory is being cleared: no processing this block
    }
    case SW_SAVE: {
        s_nostat = 8;   // core0 is paused during the commit; the output is silent, so late blocks do not matter
        s_save_blocks++;
        if (s_save_blocks == SAVE_SILENT_BLOCKS) g_sh.save_ready = 1;
        if (g_sh.save_done || s_save_blocks > SAVE_TIMEOUT_BLOCKS) {
            g_sh.save_ready = 0;
            g_sh.save_done = 0;
            init_mode(g_sh.save_mode);   // if the request moved on meanwhile, the normal check switches again
            s_in_pos = 0;
            s_sw = SW_FADE_IN;
        }
        for (int i = 0; i < n; i++) { outL[i] = 0; outR[i] = 0; }
        s_out_g1 = 0;
        return;
    }
    case SW_FADE_IN:
        s_out_pos += (float)n / (CFG_FADE_IN_OUT_MS * 0.001f * s_fs);
        s_in_pos += (float)n / (CFG_FADE_IN_IN_MS * 0.001f * s_fs);
        if (s_out_pos > 1) s_out_pos = 1;
        if (s_in_pos >= 1) { s_in_pos = 1; if (s_out_pos >= 1) s_sw = SW_RUN; }
        break;
    case SW_RUN:
        break;
    }
    s_out_g1 = scurve(s_out_pos);
    float in_g0 = scurve(s_in_pos);   // reverb-input gain (constant per block is fine: the reverb smooths it)

    // ---- input: start-up mute, impulse test, pre-delay ----
    bool muted = s_frames < (uint32_t)(CFG_STARTUP_MUTE_MS * 0.001f * s_fs);
    bool imp = false;
    if (g_sh.imp_req) muted = true;   // no live input from the request until the recording ends
    if (g_sh.imp_req == 2 && s_sw == SW_RUN && s_frames > (uint32_t)(CFG_STARTUP_MUTE_MS * 0.001f * s_fs)) {
        g_sh.imp_req = 0;
        imp = true;
        s_env_active = 1; s_env_bin = 0; s_env_k = 0; s_env_acc = 0;
        if (g_sh.tone_hz > 0) {
            s_tone = Lfo(); s_tone.set(g_sh.tone_hz, s_fs);
            s_tone_left = (int)(g_sh.tone_ms * 0.001f * s_fs);
            imp = false;
        }
        g_ir_n = 0;
        g_env_n = 0; g_env_done = 0;
    }
    if (s_env_active) muted = true;   // measure the impulse response without the input noise
    for (int i = 0; i < n; i++) {
        float l = muted ? 0.0f : inL0[i];
        float r = muted ? 0.0f : inR0[i];
        if (imp && i == 0) { l += 0.5f; r += 0.5f; }
        if (s_tone_left > 0) { s_tone.next(); l += 0.05f * s_tone.s; r += 0.05f * s_tone.s; s_tone_left--; }
        s_pre = s_pre + (pre_target - s_pre) * 0.0005f;   // slow glide (~40 ms)
        float d = s_pre < 1.0f ? 1.0f : s_pre;
        s_pdL.write(l);
        s_pdR.write(r);
        inL[i] = in_g0 * (d <= 1.0f ? l : s_pdL.readf(d));
        inR[i] = in_g0 * (d <= 1.0f ? r : s_pdR.readf(d));
    }

    s_cur->process(inL, inR, outL, outR, n, s_mp);
    if (!muted && s_sw == SW_RUN) {   // level statistics for calibrating CFG_WET_GAIN
        float a = 0, b = 0;
        for (int i = 0; i < n; i++) { a += inL0[i] * inL0[i] + inR0[i] * inR0[i]; b += outL[i] * outL[i] + outR[i] * outR[i]; }
        g_sh.in_ss = g_sh.in_ss + a;
        g_sh.rev_ss = g_sh.rev_ss + b;
        g_sh.ss_n = g_sh.ss_n + 2 * n;
    }

    // ---- impulse-response envelope (raw mode output, L) ----
    if (s_env_active) {
        for (int i = 0; i < n; i++) {
            int k = g_ir_n;
            if (k < IR_N) {
                float v = outL[i] * IR_SCALE;
                if (v > 32767.0f) v = 32767.0f;
                if (v < -32767.0f) v = -32767.0f;
                g_ir[k] = (int16_t)round_i32(v);
                g_ir_n = k + 1;
            }
            s_env_acc += outL[i] * outL[i];
            if (++s_env_k >= ENV_BIN_FRAMES) {
                g_env[s_env_bin++] = s_env_acc / ENV_BIN_FRAMES;
                g_env_n = s_env_bin;
                s_env_acc = 0; s_env_k = 0;
                if (s_env_bin >= ENV_BINS) { s_env_active = 0; g_env_done = 1; break; }
            }
        }
    }

    g_sh.rt60_now = s_cur->rt60(s_mp);
}

static inline int32_t to_word(float v) {
    float s = v * 8388607.0f;
    if (s > 8388607.0f) s = 8388607.0f;
    if (s < -8388608.0f) s = -8388608.0f;
    return (int32_t)((uint32_t)round_i32(s) << 8);
}

RAMFN void engine_run() {
    s_fs = audio_fs();
    s_arena.base = s_arena_buf;
    s_arena.cap = CFG_ARENA_FLOATS;
    s_pdL.b = s_pd_buf[0]; s_pdL.len = PD_LEN;
    s_pdR.b = s_pd_buf[1]; s_pdR.len = PD_LEN;
    s_dc_a = lp_coef(15.0f, s_fs);
    s_modes[MODE_PLATE] = mode_plate();
    s_modes[MODE_HALL] = mode_hall();
    s_modes[MODE_ROOM] = mode_room();
    s_modes[MODE_SHIMMER] = mode_shimmer();
    int m = g_sh.mode_req;
    if (m < 0 || m >= MODE_COUNT) m = 0;
    init_mode(m);
    s_sw = SW_FADE_IN; s_out_pos = 0; s_in_pos = 0;
    s_lim_att = 1.0f - lp_coef(1.0f / (CFG_LIM_ATTACK_MS * 0.001f * 6.2831853f), s_fs);    // ~time constant
    s_lim_rel = 1.0f - lp_coef(1.0f / (CFG_LIM_RELEASE_MS * 0.001f * 6.2831853f), s_fs);
    g_sh.lim_min = 1.0f;

    audio_io_start();
    busy_wait_us(500);
    const uint32_t D = (tx_rd_frame() - rx_wr_frame()) & RING_MASK;   // TX runs this far ahead
    uint32_t next = rx_wr_frame() & ~(uint32_t)(CFG_BLOCK - 1);
    const float bias_frames = CFG_BIAS_RAMP_MS * 0.001f * s_fs;
    const float lim = CFG_WET_LIMIT, inv_lim = 1.0f / CFG_WET_LIMIT;

    float inL[CFG_BLOCK], inR[CFG_BLOCK], oL[CFG_BLOCK], oR[CFG_BLOCK];
    for (;;) {
        uint32_t avail;
        while ((avail = (rx_wr_frame() - next) & RING_MASK) < CFG_BLOCK) {}
        if (avail > CFG_RING_FRAMES / 2) {     // fell far behind (e.g. a long stall): resync
            next = (rx_wr_frame() - CFG_BLOCK) & RING_MASK & ~(uint32_t)(CFG_BLOCK - 1);
            if (s_nostat == 0) g_sh.resyncs = g_sh.resyncs + 1;
        }
        uint32_t t0 = time_us_32();
        for (int i = 0; i < CFG_BLOCK; i++) {
            uint32_t f = (next + i) & RING_MASK;
            inL[i] = (float)(rx_ring[2 * f] >> 8) * (1.0f / 8388608.0f);
            inR[i] = (float)(rx_ring[2 * f + 1] >> 8) * (1.0f / 8388608.0f);
        }
        process_block(inL, inR, oL, oR);

        uint32_t wpos = next + D + CFG_OUT_AHEAD_BLK * CFG_BLOCK;
        const float gw = s_wet * CFG_WET_GAIN * MODE_WET_NORM[s_cur_idx];
        const float dg = (s_out_g1 - s_out_g0) * (1.0f / CFG_BLOCK);
        uint32_t clips = 0;
        float lim_min = 1.0f, wet_pk = 0.0f;
        for (int i = 0; i < CFG_BLOCK; i++) {
            // remove DC from the reverb output, invert (the WET stage inverts), fade, limit, soft clip
            float g = gw * (s_out_g0 + dg * (i + 1));
            float l = -s_dcL.hp(oL[i], s_dc_a) * g;
            float r = -s_dcR.hp(oR[i], s_dc_a) * g;
            float pk = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
            if (pk > wet_pk) wet_pk = pk;
            float want = (pk > LIM_THRESH && g_sh.dbg_lim) ? LIM_THRESH / pk : 1.0f;
            s_lim_gain += (want - s_lim_gain) * (want < s_lim_gain ? s_lim_att : s_lim_rel);
            if (s_lim_gain < lim_min) lim_min = s_lim_gain;
            l *= s_lim_gain;
            r *= s_lim_gain;
            if (fabsf(l) > lim || fabsf(r) > lim) clips++;
            l = lim * soft_clip(l * inv_lim);
            r = lim * soft_clip(r * inv_lim);
            float bias = CFG_DAC_BIAS;
            if (s_frames < bias_frames) bias *= fast_scurve(s_frames / bias_frames);
            s_frames++;
            uint32_t f = (wpos + i) & RING_MASK;
            tx_ring[2 * f] = to_word(bias + l);
            tx_ring[2 * f + 1] = to_word(bias + r);
        }
        bool late = ((wpos - tx_rd_frame()) & RING_MASK) > CFG_RING_FRAMES / 2;
        uint32_t dt = time_us_32() - t0;
        if (s_nostat > 0) { s_nostat--; late = false; dt = 0; }
        if (late) g_sh.underruns = g_sh.underruns + 1;
        if (dt > g_sh.load_us_max) g_sh.load_us_max = dt;
        g_sh.load_us_sum = g_sh.load_us_sum + dt;
        g_sh.load_n = g_sh.load_n + 1;
        g_sh.clips = g_sh.clips + clips;
        if (lim_min < g_sh.lim_min) g_sh.lim_min = lim_min;
        if (wet_pk > g_sh.wet_peak) g_sh.wet_peak = wet_pk;
        if (lim_min < 0.999f) g_sh.lim_blocks = g_sh.lim_blocks + 1;
        g_sh.blocks = g_sh.blocks + 1;
        next = (next + CFG_BLOCK) & RING_MASK;
    }
}
