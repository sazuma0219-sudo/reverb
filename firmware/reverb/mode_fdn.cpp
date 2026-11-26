// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shota Azuma
// HALL: 8-line feedback delay network, Householder feedback matrix, per-line damping.
//   DECAY 0.8 .. 20 s   TONE: tilt (as PLATE)   CHARACTER: size (delay lengths x0.5 .. x1.0)
// (ROOM used the same FDN until v0.5; it is now a small Dattorro tank in mode_room.cpp.)
//
// v0.4 / v0.5 against "resonant filter" ringing: one allpass inside every feedback line, line lengths
//   rounded to primes, 4 input diffusers per channel, lines x1.6 (total ~0.83 s), in-line allpasses
//   19.7 .. 44.9 ms.
// v0.10 "luxury" (2026-11-24, chosen by ear, fixed since v0.17a):
//   - bass ratio: below ~250 Hz each line decays 1.3x longer than the mids (concert halls run 1.1 .. 1.3)
//   - random wobble per line (RandMod, dsp.h; 0.29 .. 0.47 Hz, +-16 samples) instead of two shared sine
//     LFOs: same pitch swing, no pattern
//   Tried and removed in v0.17a: early reflections ("cinema-like", masked by the smoother tail).
//   history: spec/reverb_spec.md.
#include "modes.h"
#include "shared.h"

namespace {

// v0.5: lines x1.6 (modes ~1.2 Hz apart): long tails no longer ring on single notes (C5 rang)
const float LINE_MS[8] = {66.1f, 76.6f, 85.0f, 98.7f, 107.7f, 118.2f, 133.0f, 146.7f};
// v0.5: in-line allpasses at Dattorro-like scale: spectrum as smooth as PLATE
const float AP_MS[8] = {19.7f, 23.3f, 27.1f, 29.9f, 33.7f, 37.9f, 41.3f, 44.9f};
const float AP_G = 0.6f;
const float DIFF_L_MS[4] = {4.7f, 3.6f, 2.3f, 1.7f}, DIFF_R_MS[4] = {5.3f, 3.1f, 2.1f, 1.3f};
const float RT_LO = 0.8f, RT_HI = 20.0f;
const float MOD_DEPTH = 16.0f;   // samples
const float BASS_RATIO = 1.3f, BASS_XO_HZ = 250.0f;
const float RM_HZ[8] = {0.31f, 0.43f, 0.37f, 0.47f, 0.29f, 0.41f, 0.34f, 0.45f};

bool is_prime(int n) {
    if (n < 2) return false;
    for (int d = 2; d * d <= n; d++) if (n % d == 0) return false;
    return true;
}
int next_prime(int n) { while (!is_prime(n)) n++; return n; }

class Hall : public ReverbMode {
    float fs = 48828.0f;
    static const int N = 8;
    Delay line[N];
    float base[N];      // samples (prime)
    Allpass ap[N];
    OnePole lp[N];
    OnePole lo[N];      // bass-ratio crossover
    RandMod rm[N];
    OnePole bwL, bwR, hpL, hpR;   // TONE tilt on the input
    Allpass dL[4], dR[4];
    float size = 1.0f;  // smoothed size scale

    int ms(float m) const { return (int)(m * 0.001f * fs + 0.5f); }

public:
    const char *name() const override { return "hall"; }
    float rt60(const ModeParams &p) const override { return expmap(p.decay, RT_LO, RT_HI); }

    void init(Arena &a, float f) override {
        fs = f;
        for (int i = 0; i < N; i++) {
            base[i] = (float)next_prime(ms(LINE_MS[i]));
            line[i].init(a, (int)(base[i] + 2.0f * MOD_DEPTH + 4));   // read up to base + 2*md + 1
            ap[i].init(a, next_prime(ms(AP_MS[i])), AP_G);
            lp[i] = OnePole();
            lo[i] = OnePole();
            rm[i] = RandMod();
            rm[i].st = 0x9E3779B9u * (uint32_t)(i + 1) + 0x6A09E667u;
            rm[i].set(RM_HZ[i], fs);
        }
        for (int k = 0; k < 4; k++) {
            float g = k < 2 ? 0.6f : 0.5f;
            dL[k].init(a, ms(DIFF_L_MS[k]), g);
            dR[k].init(a, ms(DIFF_R_MS[k]), g);
        }
        size = 1.0f;
        bwL = OnePole(); bwR = OnePole(); hpL = OnePole(); hpR = OnePole();
    }

    RAMFN void process(const float *inL, const float *inR, float *outL, float *outR, int n,
                 const ModeParams &p) override {
        float rt = rt60(p);
        float damp = lp_coef(expmap(p.tone, 600.0f, 18000.0f), fs);
        float bwa = lp_coef(expmap(p.tone, 1500.0f, 18000.0f), fs);
        float hpa = lp_coef(p.tone > 0.5f ? expmap((p.tone - 0.5f) * 2.0f, 20.0f, 300.0f) : 20.0f, fs);
        float size_t = 0.5f + 0.5f * p.character;
        // per-block feedback gains for the current (smoothed) size; the loop = line + its allpass
        size += (size_t - size) * 0.02f;
        float g[N], len[N], glo[N];
        for (int i = 0; i < N; i++) {
            len[i] = base[i] * size;
            float loop_s = (len[i] + (float)ap[i].n) / fs;
            g[i] = rt60_gain(loop_s, rt);
            glo[i] = rt60_gain(loop_s, rt * BASS_RATIO) - g[i];   // extra gain for the low band
            if (g[i] + glo[i] > 0.9995f) glo[i] = 0.9995f - g[i];
        }
        const float xoa = lp_coef(BASS_XO_HZ, fs);
        const float md = MOD_DEPTH * g_sh.dbg_mod;
        for (int s = 0; s < n; s++) {
            float y[N], sum = 0;
            for (int i = 0; i < N; i++) {
                float m = rm[i].next();
                y[i] = line[i].readf(len[i] + md * (1.0f + m) + 1.0f);
                sum += y[i];
            }
            float h = sum * (2.0f / N);
            float il = hpL.hp(bwL.lp(inL[s], bwa), hpa);
            float ir = hpR.hp(bwR.lp(inR[s], bwa), hpa);
            float xl = dL[3].process(dL[2].process(dL[1].process(dL[0].process(il))));
            float xr = dR[3].process(dR[2].process(dR[1].process(dR[0].process(ir))));
            for (int i = 0; i < N; i++) {
                float in = (i < 4 ? xl : xr) * ((i & 1) ? -0.5f : 0.5f);
                float v = y[i] - h;
                float fb = g[i] * v + glo[i] * lo[i].lp(v, xoa);
                line[i].write(ap[i].process(in + lp[i].lp(fb, damp)));
            }
            outL[s] = 0.5f * (y[0] - y[2] + y[4] - y[6] + 0.5f * (y[1] - y[7]));
            outR[s] = 0.5f * (y[1] - y[3] + y[5] - y[7] + 0.5f * (y[2] - y[4]));
        }
    }
};

Hall s_hall;
}  // namespace

ReverbMode *mode_hall() { return &s_hall; }
