// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shota Azuma
// ROOM (v0.5, replaces the 8-line FDN): early reflections + a small Dattorro figure-8 tank.
// The FDN room kept ringing on single notes (C5 in the test loop) however its delays were tuned; the
// PLATE tank never did, so ROOM now uses the same tank with its delays and output taps scaled x0.45
// (half loop ~0.16 s instead of 0.36 s). Input diffusers keep PLATE's lengths (the FDN's 0.5 .. 2.3 ms
// diffusers were too short).
//   DECAY 0.2 .. 4 s   TONE = DISTANCE (below)   CHARACTER: early reflections vs late tail
// Aim: "wistful, nostalgic, a piano in a school music room". Chosen by ear 2026-11-25, fixed
// since v0.17a (history: spec/reverb_spec.md):
//   - static mid dip on the tail: -2.5 dB at ~460 Hz (band 300 .. 700 Hz) against "the mids pile up over
//     time" (an in-loop trim sounded "a little dry")
//   - wood warmth: +2 dB around 200 Hz (band 120 .. 350 Hz) on the whole wet, level trim -1.3 dB
//     ("the low end goes up yet the outline gets cleaner")
//   - TONE is DISTANCE: the tone filters stay at TONE = 1 (damping 18 kHz, input band 18 kHz, input
//     high-pass 300 Hz); f = 1 - TONE: 0 = near, 0.5 (knob centre) = "distant seat", 1 = further.
//     ER gain 1 - 0.8 f, ER low-pass 7 kHz x (4/7)^(2f), one more ER allpass blended in over f 0 .. 0.5,
//     level trim +2.0 dB x f (flat within ~0.5 dB over the knob). Unlike TONE in the loop (which leaves
//     only the mids at the end), this darkens only the non-recirculating early part: "naturally further".
//   Tried and removed in v0.17a: random wobble (inaudible at +-3 samples), in-loop mid trim, flutter echo
//   (metallic), wow (a "degraded piano" on synths).
#include "modes.h"
#include "shared.h"

namespace {

// early reflections: tap time ms, gain; L taps read the L input, R taps the R input (with cross-feed)
const float ER_MS_L[8] = {4.3f, 7.9f, 11.2f, 15.8f, 19.7f, 24.1f, 29.6f, 34.9f};
const float ER_MS_R[8] = {5.1f, 8.7f, 12.4f, 14.9f, 21.3f, 25.6f, 31.1f, 36.2f};
// v0.7: gains rise then fall (the old 0.84 first tap was a near copy of the dry signal)
const float ER_G[8] = {0.30f, -0.42f, 0.50f, 0.52f, -0.48f, 0.42f, -0.36f, 0.30f};

const float TANK = 0.45f;      // tank size relative to PLATE
const int MAXMOD = 12;         // spare samples in the modulated allpasses
const float EXC = 3.0f;        // fixed, gentle tank modulation (swing 0 .. 6 samples)
const float MID_LO_HZ = 300.0f, MID_HI_HZ = 700.0f;   // band = LP(700) then HP(300); |H| ~0.70 at 460 Hz
const float MID_DIP = 0.357f;                        // 1 - 0.357 * 0.70 = 0.75 = -2.5 dB
const float WOOD_LO_HZ = 120.0f, WOOD_HI_HZ = 350.0f, WOOD_K = 0.348f;   // |H| 0.745 at 205 Hz -> +2 dB
const float WOOD_TRIM = 0.861f;                      // wood added +1.3 dB to the wet rms

struct Band {   // ~300 .. 700 Hz band-pass from two one-poles (phase ~0 at the centre)
    OnePole lp, hp;
    inline float process(float x, float alp, float ahp) { float b = lp.lp(x, alp); return hp.hp(b, ahp); }
};

class Room : public ReverbMode {
    float fs = 48828.0f, S = 1.0f, ST = 1.0f;
    OnePole bw, inhp;
    Allpass in1, in2, in3, in4;
    ModAllpass mapL, mapR;
    Delay d1L, d2L, d1R, d2R;
    Allpass ap2L, ap2R;
    OnePole dampL, dampR;
    Lfo lfo;
    int n1L, n2L, n1R, n2R;
    int tL[7], tR[7];
    float half_loop_s;
    Delay erL, erR;
    int erTL[8], erTR[8];
    Allpass erdL[2], erdR[2];   // v0.7: diffuse the early reflections
    OnePole erlpL, erlpR;
    Band obL, obR;              // mid dip
    Allpass farL, farR;         // distance: extra ER smearing
    Band woodL, woodR;

    int sc(float n) const { return (int)(n * S + 0.5f); }    // input diffusers: PLATE lengths
    int st(float n) const { return (int)(n * ST + 0.5f); }   // tank: scaled
    int ms(float m) const { return (int)(m * 0.001f * fs + 0.5f); }

public:
    const char *name() const override { return "room"; }
    float rt60(const ModeParams &p) const override { return expmap(p.decay, 0.2f, 4.0f); }

    void init(Arena &a, float f) override {
        fs = f;
        S = fs / 29761.0f;
        ST = S * TANK;
        bw = OnePole(); inhp = OnePole(); dampL = OnePole(); dampR = OnePole();
        in1.init(a, sc(142), 0.75f);
        in2.init(a, sc(107), 0.75f);
        in3.init(a, sc(379), 0.625f);
        in4.init(a, sc(277), 0.625f);
        mapL.init(a, st(672), MAXMOD, -0.7f);
        mapR.init(a, st(908), MAXMOD, -0.7f);
        n1L = st(4453); n2L = st(3720); n1R = st(4217); n2R = st(3163);
        d1L.init(a, n1L); d2L.init(a, n2L); d1R.init(a, n1R); d2R.init(a, n2R);
        ap2L.init(a, st(1800), 0.5f);
        ap2R.init(a, st(2656), 0.5f);
        const int l[7] = {266, 2974, 1913, 1996, 1990, 187, 1066};
        const int r[7] = {353, 3627, 1228, 2673, 2111, 335, 121};
        for (int i = 0; i < 7; i++) { tL[i] = st(l[i]); tR[i] = st(r[i]); }
        half_loop_s = TANK * (672 + 4453 + 1800 + 3720) / 29761.0f;
        erL.init(a, ms(40.0f));
        erR.init(a, ms(40.0f));
        for (int i = 0; i < 8; i++) { erTL[i] = ms(ER_MS_L[i]); erTR[i] = ms(ER_MS_R[i]); }
        erdL[0].init(a, ms(2.9f), 0.6f); erdL[1].init(a, ms(4.7f), 0.6f);
        erdR[0].init(a, ms(3.3f), 0.6f); erdR[1].init(a, ms(5.3f), 0.6f);
        erlpL = OnePole(); erlpR = OnePole();
        lfo = Lfo();
        lfo.set(0.9f, fs);
        obL = Band(); obR = Band();
        farL.init(a, ms(7.3f), 0.65f); farR.init(a, ms(8.1f), 0.65f);
        woodL = Band(); woodR = Band();
    }

    RAMFN void process(const float *inL, const float *inR, float *outL, float *outR, int n,
                 const ModeParams &p) override {
        float decay = rt60_gain(0.5f * half_loop_s, rt60(p));   // applied twice per half loop (as PLATE)
        if (decay > 0.9995f) decay = 0.9995f;
        const float tone = 1.0f;   // TONE = DISTANCE: the tone filters stay fully open
        float damp = lp_coef(expmap(tone, 600.0f, 18000.0f), fs);
        float bwa = lp_coef(expmap(tone, 1500.0f, 18000.0f), fs);
        float hpa = lp_coef(tone > 0.5f ? expmap((tone - 0.5f) * 2.0f, 20.0f, 300.0f) : 20.0f, fs);
        float er_g = 0.10f + 0.60f * p.character, late_g = 1.0f - 0.30f * p.character;   // v0.7: more tail
        float exc = EXC * g_sh.dbg_mod;
        lfo.renorm();
        const float fd = 1.0f - p.tone;   // distance
        const float erg = er_g * (1.0f - 0.8f * fd);
        const float erla2 = lp_coef(7000.0f * fast_exp2(2.0f * fd * fast_log2(4.0f / 7.0f)), fs);
        const float apw = fd >= 0.5f ? 1.0f : 2.0f * fd;              // extra allpass blend
        const float ftrim = fast_exp2(2.0f * fd * (1.0f / 6.0206f));   // +2.0 dB x f
        const float wla = lp_coef(WOOD_HI_HZ, fs), wha = lp_coef(WOOD_LO_HZ, fs);
        const float alp = lp_coef(MID_HI_HZ, fs), ahp = lp_coef(MID_LO_HZ, fs);
        for (int i = 0; i < n; i++) {
            lfo.next();
            float mL = lfo.s, mR = lfo.c;
            float x = 0.5f * (inL[i] + inR[i]);
            x = inhp.hp(bw.lp(x, bwa), hpa);
            x = in4.process(in3.process(in2.process(in1.process(x))));

            float lastL = d2L.read(n2L);
            float lastR = d2R.read(n2R);
            float a = mapL.process(x + decay * lastR, exc * (1.0f + mL));
            float t = d1L.read(n1L);
            d1L.write(a);
            t = dampL.lp(t, damp) * decay;
            t = ap2L.process(t);
            d2L.write(t);
            float b = mapR.process(x + decay * lastL, exc * (1.0f + mR));
            float u = d1R.read(n1R);
            d1R.write(b);
            u = dampR.lp(u, damp) * decay;
            u = ap2R.process(u);
            d2R.write(u);

            float yl = d1R.read(tL[0]) + d1R.read(tL[1]) - ap2R.d.read(tL[2]) + d2R.read(tL[3])
                     - d1L.read(tL[4]) - ap2L.d.read(tL[5]) - d2L.read(tL[6]);
            float yr = d1L.read(tR[0]) + d1L.read(tR[1]) - ap2L.d.read(tR[2]) + d2L.read(tR[3])
                     - d1R.read(tR[4]) - ap2R.d.read(tR[5]) - d2R.read(tR[6]);

            yl -= MID_DIP * obL.process(yl, alp, ahp);
            yr -= MID_DIP * obR.process(yr, alp, ahp);

            erL.write(inL[i]);
            erR.write(inR[i]);
            float el = 0, er = 0;
            for (int k = 0; k < 8; k++) {
                el += ER_G[k] * ((k & 1) ? 0.7f * erL.read(erTL[k]) + 0.3f * erR.read(erTL[k]) : erL.read(erTL[k]));
                er += ER_G[k] * ((k & 1) ? 0.7f * erR.read(erTR[k]) + 0.3f * erL.read(erTR[k]) : erR.read(erTR[k]));
            }
            el = erdL[1].process(erdL[0].process(el));
            er = erdR[1].process(erdR[0].process(er));
            { float fl2 = farL.process(el), fr2 = farR.process(er); el += apw * (fl2 - el); er += apw * (fr2 - er); }
            el = erlpL.lp(el, erla2);
            er = erlpR.lp(er, erla2);
            float oL = late_g * 0.6f * yl + erg * 0.5f * el;
            float oR = late_g * 0.6f * yr + erg * 0.5f * er;
            oL += WOOD_K * woodL.process(oL, wla, wha);
            oR += WOOD_K * woodR.process(oR, wla, wha);
            oL *= ftrim; oR *= ftrim;
            oL *= WOOD_TRIM; oR *= WOOD_TRIM;
            outL[i] = oL;
            outR[i] = oR;
        }
    }
};

Room s_room;
}  // namespace

ReverbMode *mode_room() { return &s_room; }
