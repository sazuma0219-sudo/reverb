// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shota Azuma
// SHIMMER (4th mode since v0.22a, replaces SPRING: "not to my taste, beyond fine tuning", 2026-11-25).
// The HALL network (8-line Householder FDN, bass ratio, per-line random wobble); its output, pitch-shifted
// up an octave and a fifth, is fed back into its input, so every pass climbs higher (damped by TONE).
//   DECAY 0.8 .. 20 s   TONE: tilt as HALL (also how high the shifted voices climb)
//   CHARACTER: shimmer amount (0 = plain hall; full is "strong, but it does not fall apart")
// Chosen by ear 2026-11-25 (history: spec/reverb_spec.md):
//   - 4th-slot candidates were shimmer / cloud ("too far from the other modes, another time") / lo-fi
//     digital (not liked)
//   - a fifth next to the octave, 35 % of the power: "less metallic, thicker sparkle, less plugin" (octaves
//     alone only fill 2^k x; the fifth fills 1.5, 3, 4.5 .. x). Random detune (+-8 cents) was inaudible
//     (the network's own wobble masks it).
#include "modes.h"
#include "shared.h"

namespace {

const float LINE_MS[8] = {66.1f, 76.6f, 85.0f, 98.7f, 107.7f, 118.2f, 133.0f, 146.7f};   // as HALL
const float AP_MS[8] = {19.7f, 23.3f, 27.1f, 29.9f, 33.7f, 37.9f, 41.3f, 44.9f};
const float RM_HZ[8] = {0.31f, 0.43f, 0.37f, 0.47f, 0.29f, 0.41f, 0.34f, 0.45f};
const float DIFF_L_MS[4] = {4.7f, 3.6f, 2.3f, 1.7f}, DIFF_R_MS[4] = {5.3f, 3.1f, 2.1f, 1.3f};

bool is_prime(int n) {
    if (n < 2) return false;
    for (int d = 2; d * d <= n; d++) if (n % d == 0) return false;
    return true;
}
int next_prime(int n) { while (!is_prime(n)) n++; return n; }

// 8-line Householder FDN with bass ratio and per-line random wobble (the HALL core)
struct Fdn8 {
    static const int N = 8;
    Delay line[N];
    float base[N];
    Allpass ap[N];
    OnePole lp[N], lo[N];
    RandMod rm[N];
    float g[N], glo[N];
    float fsr = 48828.0f, mdmax = 16.0f;

    void init(Arena &a, float f, float scale, float apg, float mdmax_, uint32_t seed) {
        fsr = f; mdmax = mdmax_;
        for (int i = 0; i < N; i++) {
            base[i] = (float)next_prime((int)(LINE_MS[i] * scale * 0.001f * fsr + 0.5f));
            line[i].init(a, (int)(base[i] + 2.0f * mdmax + 4));
            ap[i].init(a, next_prime((int)(AP_MS[i] * scale * 0.001f * fsr + 0.5f)), apg);
            lp[i] = OnePole(); lo[i] = OnePole();
            rm[i] = RandMod();
            rm[i].st = 0x9E3779B9u * (uint32_t)(i + 1) + seed;
            rm[i].set(RM_HZ[i], fsr);
        }
    }
    void gains(float rt, float bass_ratio) {
        for (int i = 0; i < N; i++) {
            float loop_s = (base[i] + (float)ap[i].n) / fsr;
            g[i] = rt60_gain(loop_s, rt);
            glo[i] = rt60_gain(loop_s, rt * bass_ratio) - g[i];
            if (g[i] + glo[i] > 0.9995f) glo[i] = 0.9995f - g[i];
        }
    }
    // xl -> lines 0..3, xr -> lines 4..7; returns the stereo taps
    __attribute__((always_inline)) inline void tick(float xl, float xr, float damp, float xoa, float md, float &ol, float &orr) {
        float y[N], sum = 0;
        for (int i = 0; i < N; i++) {
            y[i] = line[i].readf(base[i] + md * (1.0f + rm[i].next()) + 1.0f);
            sum += y[i];
        }
        float h = sum * (2.0f / N);
        for (int i = 0; i < N; i++) {
            float in = (i < 4 ? xl : xr) * ((i & 1) ? -0.5f : 0.5f);
            float v = y[i] - h;
            float fb = g[i] * v + glo[i] * lo[i].lp(v, xoa);
            line[i].write(ap[i].process(in + lp[i].lp(fb, damp)));
        }
        ol = 0.5f * (y[0] - y[2] + y[4] - y[6] + 0.5f * (y[1] - y[7]));
        orr = 0.5f * (y[1] - y[3] + y[5] - y[7] + 0.5f * (y[2] - y[4]));
    }
};

// TONE tilt as HALL: damping 0.6 .. 18 kHz, input band 1.5 .. 18 kHz, input high-pass above the middle
struct Tilt { float damp, bwa, hpa; };
static inline Tilt tilt(float t, float fs, float top) {
    Tilt r;
    r.damp = lp_coef(expmap(t, 600.0f, top), fs);
    r.bwa = lp_coef(expmap(t, 1500.0f, top), fs);
    r.hpa = lp_coef(t > 0.5f ? expmap((t - 0.5f) * 2.0f, 20.0f, 300.0f) : 20.0f, fs);
    return r;
}

// pitch shifter tap: two taps sweep a W-sample window of a delay line (written elsewhere), sin^2 crossfade
// (constant sum). The delay falls W -> 0 while the phase runs 0 -> 1: ratio = 1 + (W - 2) dph
// (octave: dph = 1 / W).
struct Shift {
    float W = 1953.0f, ph = 0.0f, dph = 1.0f / 1953.0f;
    void init(float w, float ratio, float ph0) { W = w; dph = (ratio - 1.0f) / W; ph = ph0; }
    __attribute__((always_inline)) inline float read(const Delay &d) {
        ph += dph;
        if (ph >= 1.0f) ph -= 1.0f;
        float p2 = ph + 0.5f;
        if (p2 >= 1.0f) p2 -= 1.0f;
        float a = d.readf(1.0f + (1.0f - ph) * (W - 2.0f)), b = d.readf(1.0f + (1.0f - p2) * (W - 2.0f));
        float wa = ph < 0.5f ? fast_scurve(2.0f * ph) : fast_scurve(2.0f - 2.0f * ph);   // sin^2(pi ph)
        return a * wa + b * (1.0f - wa);
    }
};

const float SHIM_MAX = 0.55f;              // octave feedback at CHARACTER 1 (soft-clipped)
const float SHIM_HP_HZ = 250.0f, SHIM_LP_HZ = 7000.0f;   // into the shifter (no mud, no alias above fs/4)
const float SHIM_TRIM = 0.77f;    // wet rms matched to the old SPRING (test loop, CHARACTER 1, DECAY 0.78)
const float FIFTH_R = 0.01f * 35.0f;   // fifth share of the shifted power, 35 % (written as the old `dbg shim 35`)
const float W_OCT = sqrtf(1.0f - FIFTH_R), W_FIFTH = sqrtf(FIFTH_R);

class Shimmer : public ReverbMode {
    float fs = 48828.0f;
    Fdn8 f;
    OnePole bwL, bwR, hpL, hpR;
    Allpass dL[4], dR[4];
    Delay bL, bR;                 // shifter input (shared by the octave and fifth taps)
    Shift ocL, ocR, fiL, fiR;
    OnePole s1L, s2L, shpL, s1R, s2R, shpR;
    float fbL = 0, fbR = 0;
    int ms(float m) const { return (int)(m * 0.001f * fs + 0.5f); }

public:
    const char *name() const override { return "shimmer"; }
    float rt60(const ModeParams &p) const override { return expmap(p.decay, 0.8f, 20.0f); }
    void init(Arena &a, float fsx) override {
        fs = fsx;
        f.init(a, fs, 1.0f, 0.6f, 16.0f, 0x6A09E667u);
        for (int k = 0; k < 4; k++) {
            float g = k < 2 ? 0.6f : 0.5f;
            dL[k].init(a, ms(DIFF_L_MS[k]), g);
            dR[k].init(a, ms(DIFF_R_MS[k]), g);
        }
        // slightly different windows L / R: the grain seams do not line up
        float wl = 40.0f * 0.001f * fs, wr = 43.0f * 0.001f * fs;
        bL.init(a, (int)wl + 4); bR.init(a, (int)wr + 4);
        ocL.init(wl, 2.0f, 0.0f); ocL.dph = 1.0f / wl;
        ocR.init(wr, 2.0f, 0.0f); ocR.dph = 1.0f / wr;
        fiL.init(wl, 1.4983f, 0.25f); fiR.init(wr, 1.4983f, 0.25f);   // 2^(7/12)
        bwL = OnePole(); bwR = OnePole(); hpL = OnePole(); hpR = OnePole();
        s1L = OnePole(); s2L = OnePole(); shpL = OnePole(); s1R = OnePole(); s2R = OnePole(); shpR = OnePole();
        fbL = fbR = 0;
    }
    RAMFN void process(const float *inL, const float *inR, float *outL, float *outR, int n,
                       const ModeParams &p) override {
        f.gains(rt60(p), 1.3f);
        Tilt t = tilt(p.tone, fs, 18000.0f);
        const float xoa = lp_coef(250.0f, fs), md = 16.0f * g_sh.dbg_mod;
        const float amt = SHIM_MAX * p.character;
        const float sla = lp_coef(SHIM_LP_HZ, fs), sha = lp_coef(SHIM_HP_HZ, fs);
        const float wo = W_OCT, wf = W_FIFTH;
        for (int s = 0; s < n; s++) {
            float il = hpL.hp(bwL.lp(inL[s], t.bwa), t.hpa) + amt * fbL;
            float ir = hpR.hp(bwR.lp(inR[s], t.bwa), t.hpa) + amt * fbR;
            float xl = dL[3].process(dL[2].process(dL[1].process(dL[0].process(il))));
            float xr = dR[3].process(dR[2].process(dR[1].process(dR[0].process(ir))));
            float ol, orr;
            f.tick(xl, xr, t.damp, xoa, md, ol, orr);
            bL.write(shpL.hp(s2L.lp(s1L.lp(ol, sla), sla), sha));
            bR.write(shpR.hp(s2R.lp(s1R.lp(orr, sla), sla), sha));
            float vl = wo * ocL.read(bL) + wf * fiL.read(bL);
            float vr = wo * ocR.read(bR) + wf * fiR.read(bR);
            // L shifted voice goes back in on the R side and vice versa (wider)
            fbR = soft_clip(vl);
            fbL = soft_clip(vr);
            outL[s] = SHIM_TRIM * ol;
            outR[s] = SHIM_TRIM * orr;
        }
    }
};

Shimmer s_shim;
}  // namespace

ReverbMode *mode_shimmer() { return &s_shim; }
