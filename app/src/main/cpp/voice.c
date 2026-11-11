#include "voice.h"
#include <math.h>
#include <string.h>

#define F0_MIN      60.0f
#define F0_MAX     420.0f
#define YIN_W      2048        // YIN integration window
#define YIN_THRESH 0.35f
#define OT_FMIN    VOICE_OT_FMIN
#define OT_FMAX    VOICE_OT_FMAX

static float re_[VOICE_N], im_[VOICE_N], mag_[VOICE_N/2];

static void fft(float *re, float *im, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f*3.14159265f/len;
        float wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len/2; k++) {
                int a = i + k, b = a + len/2;
                float xr = re[b]*cr - im[b]*ci, xi = re[b]*ci + im[b]*cr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr;        im[a] += xi;
                float t = cr*wr - ci*wi; ci = cr*wi + ci*wr; cr = t;
            }
        }
    }
}

// YIN difference function; collects local minima (pitch-period candidates)
static int yinCandidates(const float *x, float sr, float *cand, float *cval, int maxc)
{
    static float d[1024];
    int tmin = (int)(sr/F0_MAX), tmax = (int)(sr/F0_MIN);
    if (tmax > 1022) tmax = 1022;
    const float *s = x + VOICE_N - YIN_W - tmax;
    d[0] = 1; float run = 0;
    for (int t = 1; t <= tmax + 1; t++) {
        float acc = 0;
        for (int i = 0; i < YIN_W; i++) { float v = s[i] - s[i + t]; acc += v*v; }
        run += acc;
        d[t] = run > 0 ? acc*t/run : 1;
    }
    int n = 0;
    for (int t = 2; t <= tmax && n < maxc; t++) {
        if (d[t] < YIN_THRESH && d[t] <= d[t-1] && d[t] <= d[t+1]) {
            float a = d[t-1], b = d[t], c = d[t+1], den = a - 2*b + c;
            float off = fabsf(den) > 1e-9f ? 0.5f*(a - c)/den : 0;
            float f = sr/(t + off);
            if (f >= F0_MIN && f <= F0_MAX) { cand[n] = f; cval[n] = b; n++; }
        }
    }
    (void)tmin;
    return n;
}

static float harmEnergy(float f, float binHz)
{
    int k0 = (int)(f*0.97f/binHz), k1 = (int)(f*1.03f/binHz + 1);
    if (k1 >= VOICE_N/2) return 0;
    if (k1 - k0 < 2) { k0--; k1++; }
    float m = 0;
    for (int k = k0; k <= k1; k++) if (mag_[k] > m) m = mag_[k];
    return m;
}

static float troughEnergy(float f, float binHz)
{
    int k = (int)(f/binHz + 0.5f);
    if (k + 1 >= VOICE_N/2) return 0;
    return (mag_[k-1] + mag_[k] + mag_[k+1])/3;
}

// mean peak-vs-trough contrast (dB) over harmonics of f
static float harmonicity(float f, float binHz)
{
    float s = 0; int n = 0;
    for (int h = 1; h*f < 3000.0f && h <= 20; h++) {
        float p = harmEnergy(h*f, binHz), q = troughEnergy((h + 0.5f)*f, binHz);
        s += 20*log10f((p + 1e-9f)/(q + 1e-9f)); n++;
    }
    return n ? s/n : 0;
}

VoiceInfo VoiceAnalyze(const float *x, float sr)
{
    VoiceInfo v; memset(&v, 0, sizeof v);
    double e = 0;
    for (int i = VOICE_N - 2048; i < VOICE_N; i++) e += x[i]*x[i];
    v.rms = (float)sqrt(e/2048);

    if (v.rms < 1e-4f) return v;

    for (int i = 0; i < VOICE_N; i++) {
        float w = 0.5f - 0.5f*cosf(2*3.14159265f*i/(VOICE_N - 1));
        re_[i] = x[i]*w; im_[i] = 0;
    }
    fft(re_, im_, VOICE_N);
    for (int k = 0; k < VOICE_N/2; k++) mag_[k] = sqrtf(re_[k]*re_[k] + im_[k]*im_[k]);
    float binHz = sr/VOICE_N;

    // coarse spectrum for display
    for (int b = 0; b < VOICE_SPEC_BINS; b++) {
        int k0 = (int)(b*VOICE_SPEC_FMAX/VOICE_SPEC_BINS/binHz);
        int k1 = (int)((b + 1)*VOICE_SPEC_FMAX/VOICE_SPEC_BINS/binHz);
        if (k1 >= VOICE_N/2) k1 = VOICE_N/2 - 1;
        float m = 0;
        for (int k = k0; k <= k1; k++) if (mag_[k] > m) m = mag_[k];
        v.spec[b] = 20*log10f(m + 1e-9f);
    }
    v.hasSpec = 1;
    {   // strongest peak, parabolic interpolation
        int k0 = (int)(50/binHz), k1 = (int)(4000/binHz), bk = k0;
        for (int k = k0; k <= k1; k++) if (mag_[k] > mag_[bk]) bk = k;
        float a = mag_[bk-1], b = mag_[bk], c = mag_[bk+1], den = a - 2*b + c;
        v.peakFreq = (bk + (den != 0 ? 0.5f*(a - c)/den : 0))*binHz;
    }

    // A dominant sung overtone makes YIN lock onto its period. Take all YIN
    // minima and keep the lowest frequency whose harmonic comb fits the spectrum
    // about as well as the best one (sub-octaves fit badly, overtones equally well).
    float cand[32], cval[32], sc[32];
    int nc = yinCandidates(x, sr, cand, cval, 32);
    if (nc == 0) return v;
    float best = -1e9f;
    for (int i = 0; i < nc; i++) { sc[i] = harmonicity(cand[i], binHz); if (sc[i] > best) best = sc[i]; }
    if (best < 6.0f) return v;                  // not harmonic enough: noise / unvoiced
    float f0 = 1e9f;
    for (int i = 0; i < nc; i++) if (sc[i] >= best - 3.0f && cand[i] < f0) f0 = cand[i];
    v.f0 = f0;
    v.voiced = 1;

    for (int n = 1; n <= VOICE_HMAX && n*f0 < VOICE_SPEC_FMAX; n++) {
        v.harmDb[n-1] = 20*log10f(harmEnergy(n*f0, binHz) + 1e-9f);
        v.nHarm = n;
    }

    float L[64]; int ns[64], cnt = 0;
    for (int n = 2; n < 64; n++) {
        float f = n*v.f0;
        if (f > OT_FMAX) break;
        if (f < OT_FMIN) continue;
        ns[cnt] = n; L[cnt] = 20*log10f(harmEnergy(f, binHz) + 1e-9f); cnt++;
    }
    if (cnt < 3) return v;

    // sung overtone = one harmonic far above its neighbours (formants are broad)
    int bi = 0; float bestProm = -1e9f;
    for (int i = 0; i < cnt; i++) {
        float nb[6]; int m = 0;
        for (int j = i - 3; j <= i + 3; j++) if (j != i && j >= 0 && j < cnt) nb[m++] = L[j];
        for (int a = 1; a < m; a++) for (int b = a; b > 0 && nb[b] < nb[b-1]; b--) { float t = nb[b]; nb[b] = nb[b-1]; nb[b-1] = t; }
        float prom = L[i] - (m ? nb[m/2] : L[i]);
        if (prom > bestProm) { bestProm = prom; bi = i; }
    }
    v.otProm = bestProm;
    v.otN = ns[bi];

    // continuous harmonic number: power-weighted centroid of peak and neighbours
    double sw = 0, sn = 0;
    for (int i = bi - 1; i <= bi + 1; i++) {
        if (i < 0 || i >= cnt) continue;
        double p = pow(10.0, L[i]/10.0);
        sw += p; sn += p*ns[i];
    }
    v.otHarm = (float)(sn/sw);
    v.otFreq = v.otHarm*v.f0;
    return v;
}
