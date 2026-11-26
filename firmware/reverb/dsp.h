// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shota Azuma
// DSP building blocks shared by the reverb modes. All float, per-sample inline.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

// Place hot DSP code in SRAM (XIP flash cache is shared with core1 and too small)
#define RAMFN __attribute__((section(".time_critical.rvb"), noinline))

// ---- small math, inlined into the RAM code -----------------------------------------------------
// newlib's powf / expf / cosf / lrintf live in flash. The XIP cache is shared by both cores, and console
// output on the other core evicts them: audio blocks then stretched to ~1 ms (v0.4 investigation).
#define FASTINL static inline __attribute__((always_inline))

// 2^x, |rel. error| < 2e-7 for x in [-126, 126]
FASTINL float fast_exp2(float x) {
    if (x < -126.0f) x = -126.0f;
    if (x > 126.0f) x = 126.0f;
    float fi = (float)(int)x;
    if (fi > x) fi -= 1.0f;            // floor
    float f = x - fi;                  // 0 .. 1
    // minimax polynomial for 2^f on [0, 1]
    float p = 1.8775767e-3f;
    p = p * f + 8.9893397e-3f;
    p = p * f + 5.5826318e-2f;
    p = p * f + 2.4015361e-1f;
    p = p * f + 6.9315308e-1f;
    p = p * f + 9.9999994e-1f;
    union { float f; int32_t i; } u;
    u.i = ((int32_t)fi + 127) << 23;
    return p * u.f;
}
// log2(x), x > 0, |abs. error| < 1e-5
FASTINL float fast_log2(float x) {
    union { float f; int32_t i; } u;
    u.f = x;
    float e = (float)(((u.i >> 23) & 255) - 128);
    u.i = (u.i & 0x007FFFFF) | 0x3F800000;   // mantissa in [1, 2)
    float m = u.f;
    // log2(m) = 2 atanh(z) / ln2 with z = (m - 1) / (m + 1), |z| <= 1/3 (series to z^7)
    float z = (m - 1.0f) / (m + 1.0f), z2 = z * z;
    float a = z * (2.8853901f + z2 * (0.9617967f + z2 * (0.5770780f + z2 * 0.4121983f)));
    return e + 1.0f + a;
}
FASTINL float fast_exp(float x) { return fast_exp2(x * 1.44269504f); }
// raised-cosine fade 0..1 (same shape as 0.5 - 0.5 cos(pi p)), via a 6th-order polynomial
FASTINL float fast_scurve(float p) {
    if (p <= 0.0f) return 0.0f;
    if (p >= 1.0f) return 1.0f;
    float x = 3.14159265f * (p - 0.5f);           // -pi/2 .. pi/2 ; 0.5 + 0.5 sin(x)
    float x2 = x * x;
    float sn = x * (1.0f - x2 * (0.16666667f - x2 * (0.0083333310f - x2 * 0.00019840874f)));
    return 0.5f + 0.5f * sn;
}
FASTINL int32_t round_i32(float s) { return (int32_t)(s >= 0.0f ? s + 0.5f : s - 0.5f); }

// Linear allocator over one static buffer; reset on every mode change.
// alloc() does not clear: the engine zeroes the used region in chunks while the wet signal is muted
// (clearing up to 148 KB in one block took ~1.1 ms of the ~1.3 ms the output can wait).
struct Arena {
    float *base = nullptr;
    int cap = 0, used = 0;
    bool fail = false;
    void reset() { used = 0; fail = false; }
    float *alloc(int n) {
        if (used + n > cap) { fail = true; return nullptr; }
        float *p = base + used;
        used += n;
        return p;
    }
};

// Circular delay line. read(d) returns the sample written d writes ago (call before write()).
struct Delay {
    float *b = nullptr;
    int len = 1, w = 0;
    float dummy[4] = {0, 0, 0, 0};
    void init(Arena &a, int n) {
        b = a.alloc(n);
        len = n;
        w = 0;
        if (!b) { b = dummy; len = 4; }
    }
    inline float read(int d) const {
        int i = w - d;
        if (i < 0) i += len;
        return b[i];
    }
    inline float readf(float d) const {   // linear interpolation, 1 <= d <= len - 1
        int di = (int)d;
        float f = d - (float)di;
        float x0 = read(di), x1 = read(di + 1);
        return x0 + (x1 - x0) * f;
    }
    inline void write(float x) {
        b[w] = x;
        if (++w >= len) w = 0;
    }
};

// Schroeder allpass: v = x - g*z, y = z + g*v (z = v delayed by n)
struct Allpass {
    Delay d;
    int n = 1;
    float g = 0.5f;
    void init(Arena &a, int len, float gg) { n = len; g = gg; d.init(a, len); }
    inline float process(float x) {
        float z = d.read(n);
        float v = x - g * z;
        d.write(v);
        return z + g * v;
    }
};

struct ModAllpass {
    Delay d;
    float n = 1;
    float g = 0.5f;
    void init(Arena &a, int len, int maxmod, float gg) { n = (float)len; g = gg; d.init(a, len + maxmod + 2); }
    inline float process(float x, float mod) {
        float z = d.readf(n + mod);
        float v = x - g * z;
        d.write(v);
        return z + g * v;
    }
};

// one-pole low-pass, a = exp(-2 pi fc / fs)
struct OnePole {
    float z = 0;
    inline float lp(float x, float a) { z = x + a * (z - x); return z; }
    inline float hp(float x, float a) { return x - lp(x, a); }
};

static inline float lp_coef(float fc, float fs) { return fast_exp(-6.2831853f * fc / fs); }

// rational tanh-like soft clip, unity slope at 0, saturates at +-1
FASTINL float soft_clip(float x) {
    if (x > 3.0f) x = 3.0f;
    if (x < -3.0f) x = -3.0f;
    return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}

// quadrature sine oscillator (renormalise once per block)
struct Lfo {
    float s = 0, c = 1, ks = 0, kc = 1;
    // w is tiny (< 1e-3 rad for LFO rates): 2nd / 3rd-order Taylor terms are exact to float precision
    void set(float hz, float fs) { float w = 6.2831853f * hz / fs; kc = 1.0f - 0.5f * w * w; ks = w * (1.0f - w * w * (1.0f / 6.0f)); }
    inline void next() { float ns = s * kc + c * ks; c = c * kc - s * ks; s = ns; }
    void renorm() { float r = 1.0f / sqrtf(s * s + c * c); s *= r; c *= r; }
};

// Random wobble (PLATE v0.9d, HALL v0.10a): half-cosine swings between alternating peaks (like the sine), but each peak is a
// random 60 .. 100 % of full depth and each half-cycle a random length (+-30 %, mean 0.82 x the sine's).
// Simulated: rms rate of change 7.2 /s vs the sine's 7.1 /s at 1.6 Hz (= the same pitch swing), level
// rms 0.58 vs 0.71. (v0.9a's version moved ~4.5x slower -> it really just made the wobble weaker.)
struct RandMod {
    uint32_t st = 1;
    float from = 0, to = 1, ph = 0, dph = 0.0001f, half = 15000;
    inline float rnd01() { st ^= st << 13; st ^= st >> 17; st ^= st << 5; return (float)(st >> 8) * (1.0f / 16777216.0f); }
    void set(float hz, float fs) { half = 0.82f * fs / (2.0f * hz); }
    inline float next() {
        ph += dph;
        if (ph >= 1.0f) {
            ph = 0.0f;
            from = to;
            float amp = 0.6f + 0.4f * rnd01();
            to = from > 0.0f ? -amp : amp;
            dph = 1.0f / (half * (1.0f + 0.3f * (2.0f * rnd01() - 1.0f)));
        }
        return from + (to - from) * fast_scurve(ph);
    }
};

// exponential map of 0..1 onto [lo, hi]
static inline float expmap(float p, float lo, float hi) { return lo * fast_exp2(p * fast_log2(hi / lo)); }

// feedback gain for a loop of `seconds` so that the tail falls 60 dB in rt60 seconds
static inline float rt60_gain(float seconds, float rt60) { return fast_exp2(-3.0f * 3.32192809f * seconds / rt60); }

// Parameters handed to every mode once per block (all 0..1, already smoothed)
struct ModeParams {
    float decay, tone, character;
};

class ReverbMode {
public:
    virtual ~ReverbMode() {}
    virtual const char *name() const = 0;
    virtual void init(Arena &a, float fs) = 0;
    virtual void process(const float *inL, const float *inR, float *outL, float *outR, int n,
                         const ModeParams &p) = 0;
    virtual float rt60(const ModeParams &p) const = 0;   // for the console
};
