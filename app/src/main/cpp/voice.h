// Voice analysis for overtone singing: fundamental (YIN) + dominant overtone (FFT)
#ifndef VOICE_H
#define VOICE_H

#define VOICE_N 4096            // analysis window (samples)
#define VOICE_OT_FMIN   450.0f  // band where sung overtones are searched
#define VOICE_OT_FMAX  4000.0f
#define VOICE_SPEC_BINS  160    // coarse spectrum for display, 0..VOICE_SPEC_FMAX
#define VOICE_SPEC_FMAX 4000.0f
#define VOICE_HMAX        48    // harmonic levels reported for display

typedef struct {
    int   voiced;               // 1 if a stable pitched voice is present
    float rms;                  // signal level
    float f0;                   // fundamental, Hz
    float otFreq;               // dominant overtone frequency, Hz
    float otHarm;               // its harmonic number (continuous)
    float otProm;               // its prominence over other harmonics, dB
    int   otN;                  // its harmonic number (integer peak), 0 if none
    int   nHarm;                // harmonics 1..nHarm below VOICE_SPEC_FMAX are in harmDb
    float harmDb[VOICE_HMAX];   // level of harmonic n at harmDb[n-1], dB
    int   hasSpec;              // spec[] is valid
    float spec[VOICE_SPEC_BINS];// peak level per display bin, dB
} VoiceInfo;

// x: last VOICE_N samples (mono), sr: sample rate
VoiceInfo VoiceAnalyze(const float *x, float sr);

#endif
