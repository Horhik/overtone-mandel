#include "beat.h"
#include <math.h>
#include <string.h>

#define HOP      512                // samples per onset-envelope step (~94 Hz at 48 kHz)
#define ENV_N    1024               // envelope history (~11 s)
#define WIN      512                // envelope steps used for tempo (~5.5 s)
#define BPM_LO   60.0f
#define BPM_HI   180.0f
#define UPDATE   24                 // re-estimate tempo every UPDATE hops (~0.25 s)

static float sr = 48000, rate;      // rate: envelope steps per second
static float hop[HOP]; static int hopN;
static float env[ENV_N]; static unsigned envW;
static float lp, ePrev, eSlow;
static float bpm, conf, phase;      // phase advances per hop, corrected on each update
static float candBpm; static int candCnt;
static int sinceUpdate;

void BeatInit(float sampleRate)
{
    sr = sampleRate; rate = sr/HOP;
    hopN = 0; envW = 0; lp = ePrev = eSlow = 0;
    bpm = conf = phase = 0; candBpm = 0; candCnt = 0; sinceUpdate = 0;
    memset(env, 0, sizeof env);
}

static float E(int back) { return env[(envW - 1 - back) & (ENV_N - 1)]; }   // back = 0: newest

static void Estimate(void)
{
    if (envW < WIN) return;
    int lagLo = (int)(rate*60/BPM_HI), lagHi = (int)(rate*60/BPM_LO) + 1;
    float mean = 0, energy = 0;
    for (int i = 0; i < WIN; i++) mean += E(i);
    mean /= WIN;
    for (int i = 0; i < WIN; i++) { float d = E(i) - mean; energy += d*d; }
    if (energy < 1e-6f) { conf *= 0.9f; return; }

    // autocorrelation of the mean-removed onset envelope, plus the double lag (helps pick the beat,
    // not an off-beat), weighted by a soft prior around 120 BPM
    static float ac[256];
    int best = 0; float bestV = -1e9f;
    for (int L = lagLo; L <= lagHi && L < 256; L++) {
        float s = 0, s2 = 0;
        for (int i = 0; i + L < WIN; i++) s += (E(i) - mean)*(E(i + L) - mean);
        for (int i = 0; i + 2*L < WIN; i++) s2 += (E(i) - mean)*(E(i + 2*L) - mean);
        float b = 60*rate/L, o = log2f(b/120.0f);
        ac[L] = (s + 0.5f*s2)/energy*expf(-0.5f*o*o/(0.9f*0.9f));
        if (ac[L] > bestV) { bestV = ac[L]; best = L; }
    }
    if (best <= lagLo || best >= lagHi) { conf *= 0.9f; return; }
    float a = ac[best-1], b = ac[best], c = ac[best+1];          // parabolic peak -> fractional lag
    float den = a - 2*b + c, frac = den != 0 ? 0.5f*(a - c)/den : 0;
    float lag = best + frac, nb = 60*rate/lag;
    float nc = bestV < 0 ? 0 : bestV > 1 ? 1 : bestV;
    conf += (nc - conf)*0.3f;

    // tempo hysteresis: a different tempo must win three updates in a row
    if (bpm == 0 || fabsf(nb/bpm - 1) < 0.04f) { bpm = bpm == 0 ? nb : bpm + (nb - bpm)*0.2f; candCnt = 0; }
    else if (candBpm > 0 && fabsf(nb/candBpm - 1) < 0.04f) { if (++candCnt >= 3) { bpm = nb; candCnt = 0; } }
    else { candBpm = nb; candCnt = 1; }

    // phase: where a comb with the beat period lines up best with the newest onsets
    float period = 60*rate/bpm;
    int P = (int)(period + 0.5f);
    float bestS = -1e9f; int bestOff = 0;
    for (int off = 0; off < P; off++) {
        float s = 0;
        for (int k = 0; k < 8; k++) {
            int i = off + (int)(k*period + 0.5f);
            if (i >= ENV_N) break;
            s += E(i)*(1.0f - 0.08f*k);
        }
        if (s > bestS) { bestS = s; bestOff = off; }
    }
    float target = bestOff/period;                 // the beat was bestOff hops ago
    float d = target - phase;
    d -= floorf(d + 0.5f);                          // shortest way round the circle
    phase += 0.35f*d;
    phase -= floorf(phase);
}

static void Hop(void)
{
    // onset strength: rise of log energy, low end (kick) weighted up
    float e = 0, el = 0;
    for (int i = 0; i < HOP; i++) {
        float x = hop[i];
        lp += (x - lp)*0.02f;                       // ~150 Hz one-pole low-pass
        e += x*x; el += lp*lp;
    }
    float le = logf(1e-7f + e/HOP + 4*el/HOP);
    eSlow += (le - eSlow)*0.1f;
    float on = le - (ePrev > eSlow ? ePrev : eSlow);
    ePrev = le;
    env[envW & (ENV_N - 1)] = on > 0 ? on : 0;
    envW++;

    if (bpm > 0) { phase += bpm/60.0f/rate; phase -= floorf(phase); }
    if (++sinceUpdate >= UPDATE) { sinceUpdate = 0; Estimate(); }
}

void BeatPush(const float *x, int n)
{
    for (int i = 0; i < n; i++) {
        hop[hopN++] = x[i];
        if (hopN == HOP) { hopN = 0; Hop(); }
    }
}

BeatInfo BeatGet(void) { return (BeatInfo){ bpm, conf, phase }; }
