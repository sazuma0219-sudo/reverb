// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shota Azuma
// PLATE: Dattorro figure-8 tank (J. Dattorro, "Effect Design Part 1", JAES 1997).
// Lengths are the paper's (29761 Hz) scaled to fs.
//   DECAY 0.4 .. 15 s
//   TONE (tilt): tank damping 0.6 .. 18 kHz and input band-limit 1.5 .. 18 kHz; above the middle the input
//         low end is also thinned (high-pass 20 .. 300 Hz) -> left = dark / thick, right = bright / thin
//   CHARACTER: left half = diffusion (0 = grainy, echo-like; 0.5 = Dattorro's values),
//              right half = tank modulation (depth 0 .. +-50 samples, rate 0.6 .. 1.6 Hz)
// v0.9 "rawer, less like a soft synth" (2026-11-24, chosen by ear, fixed since v0.17a):
//   - random wobble (RandMod, dsp.h) instead of the sine LFO, independent per side, same pitch swing
//   - gentle saturation on the drive and in each tank half: drive 1.5, asymmetric 'warm' curve (bias 0.5,
//     even harmonics; small-signal gain 1 so the decay time does not change) + a 5 Hz DC blocker per half
//   Tried and removed in v0.17a: dispersion (brighter, not wanted), symmetric curve, other drives
//   (higher = muddier). history: spec/reverb_spec.md.
#include "modes.h"
#include "shared.h"

namespace {

// y = (clip(d x + b) - clip(b)) / (d clip'(b)); b = 0 is the symmetric curve. Unity slope at 0.
struct Sat {
    float d = 1.5f, b = 0.0f, off = 0.0f, inv = 1.0f / 1.5f;
    void set(float drive, float bias) {
        d = drive; b = bias; off = soft_clip(b);
        const float e = 1e-3f;
        float slope = (soft_clip(b + e) - soft_clip(b - e)) / (2.0f * e);
        inv = 1.0f / (d * slope);
    }
    inline float process(float x) const { return (soft_clip(d * x + b) - off) * inv; }
};

const float SAT_DRIVE = 1.5f, WARM_BIAS = 0.5f;

class Plate : public ReverbMode {
    float fs = 48828.0f, S = 1.0f;
    OnePole bw, inhp;
    Allpass in1, in2, in3, in4;
    ModAllpass mapL, mapR;
    Delay d1L, d2L, d1R, d2R;
    Allpass ap2L, ap2R;
    OnePole dampL, dampR;
    RandMod rmL, rmR;
    Sat sat;
    OnePole dcL, dcR;
    int n1L, n2L, n1R, n2R;
    int tL[7], tR[7];
    float half_loop_s;

    static const int MAXMOD = 110;   // spare samples in the modulated allpasses (swing 0 .. 2 x 50)

    int sc(float n) const { return (int)(n * S + 0.5f); }

public:
    const char *name() const override { return "plate"; }

    float rt60(const ModeParams &p) const override { return expmap(p.decay, 0.4f, 15.0f); }

    void init(Arena &a, float f) override {
        fs = f;
        S = fs / 29761.0f;
        bw = OnePole(); inhp = OnePole(); dampL = OnePole(); dampR = OnePole();
        in1.init(a, sc(142), 0.75f);
        in2.init(a, sc(107), 0.75f);
        in3.init(a, sc(379), 0.625f);
        in4.init(a, sc(277), 0.625f);
        mapL.init(a, sc(672), MAXMOD, -0.7f);
        mapR.init(a, sc(908), MAXMOD, -0.7f);
        n1L = sc(4453); n2L = sc(3720); n1R = sc(4217); n2R = sc(3163);
        d1L.init(a, n1L); d2L.init(a, n2L); d1R.init(a, n1R); d2R.init(a, n2R);
        ap2L.init(a, sc(1800), 0.5f);
        ap2R.init(a, sc(2656), 0.5f);
        const int l[7] = {266, 2974, 1913, 1996, 1990, 187, 1066};
        const int r[7] = {353, 3627, 1228, 2673, 2111, 335, 121};
        for (int i = 0; i < 7; i++) { tL[i] = sc(l[i]); tR[i] = sc(r[i]); }
        half_loop_s = (672 + 4453 + 1800 + 3720) / 29761.0f;
        rmL = RandMod(); rmR = RandMod();
        dcL = OnePole(); dcR = OnePole();
        rmL.st = 0x9E3779B9u; rmR.st = 0x7F4A7C15u;
        sat.set(SAT_DRIVE, WARM_BIAS);
    }

    RAMFN void process(const float *inL, const float *inR, float *outL, float *outR, int n,
                 const ModeParams &p) override {
        // `decay` is applied twice per half loop (after the damping and on the cross-feed), so each
        // application gets half of the half-loop's attenuation (measured 1.27 s vs 2.45 s before this)
        float decay = rt60_gain(0.5f * half_loop_s, rt60(p));
        if (decay > 0.9995f) decay = 0.9995f;
        // TONE: tilt
        float fc = expmap(p.tone, 600.0f, 18000.0f);
        float damp = lp_coef(fc, fs);
        float bwa = lp_coef(expmap(p.tone, 1500.0f, 18000.0f), fs);
        float hpf = p.tone > 0.5f ? expmap((p.tone - 0.5f) * 2.0f, 20.0f, 300.0f) : 20.0f;
        float hpa = lp_coef(hpf, fs);
        // CHARACTER: diffusion on the left half, modulation on the right half
        float c = p.character;
        float dif = c < 0.5f ? 0.15f + 0.85f * (c * 2.0f) : 1.0f;   // 0.15 .. 1 x Dattorro's coefficients
        in1.g = in2.g = 0.75f * dif;
        in3.g = in4.g = 0.625f * dif;
        mapL.g = mapR.g = -0.7f * dif;
        ap2L.g = ap2R.g = 0.5f * dif;
        float m = c > 0.5f ? (c - 0.5f) * 2.0f : 0.0f;
        float exc = 25.0f * m * g_sh.dbg_mod;          // swing 0 .. 2*exc = 0 .. 50 samples (<= MAXMOD)
        float rate = 0.6f + 1.0f * m;
        const float dca = lp_coef(5.0f, fs);
        rmL.set(rate, fs);
        rmR.set(rate, fs);
        for (int i = 0; i < n; i++) {
            float mL = rmL.next(), mR = rmR.next();
            float x = sat.process(0.5f * (inL[i] + inR[i]));
            x = inhp.hp(bw.lp(x, bwa), hpa);
            x = in4.process(in3.process(in2.process(in1.process(x))));

            float lastL = d2L.read(n2L);
            float lastR = d2R.read(n2R);
            // left half
            float a = mapL.process(x + decay * lastR, exc * (1.0f + mL));
            float t = d1L.read(n1L);
            d1L.write(a);
            t = dampL.lp(t, damp) * decay;
            t = dcL.hp(sat.process(t), dca);
            t = ap2L.process(t);
            d2L.write(t);
            // right half
            float b = mapR.process(x + decay * lastL, exc * (1.0f + mR));
            float u = d1R.read(n1R);
            d1R.write(b);
            u = dampR.lp(u, damp) * decay;
            u = dcR.hp(sat.process(u), dca);
            u = ap2R.process(u);
            d2R.write(u);

            float yl = d1R.read(tL[0]) + d1R.read(tL[1]) - ap2R.d.read(tL[2]) + d2R.read(tL[3])
                     - d1L.read(tL[4]) - ap2L.d.read(tL[5]) - d2L.read(tL[6]);
            float yr = d1L.read(tR[0]) + d1L.read(tR[1]) - ap2L.d.read(tR[2]) + d2L.read(tR[3])
                     - d1R.read(tR[4]) - ap2R.d.read(tR[5]) - d2R.read(tR[6]);
            outL[i] = 0.6f * yl;
            outR[i] = 0.6f * yr;
        }
    }
};

Plate s_plate;
}  // namespace

ReverbMode *mode_plate() { return &s_plate; }
