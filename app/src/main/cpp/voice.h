// Voice analysis for overtone singing: fundamental (YIN) + dominant overtone (FFT)
#ifndef VOICE_H
#define VOICE_H

#define VOICE_N 4096            // analysis window (samples)

typedef struct {
    int   voiced;               // 1 if a stable pitched voice is present
    float rms;                  // signal level
    float f0;                   // fundamental, Hz
    float otFreq;               // dominant overtone frequency, Hz
    float otHarm;               // its harmonic number (continuous)
    float otProm;               // its prominence over other harmonics, dB
} VoiceInfo;

// x: last VOICE_N samples (mono), sr: sample rate
VoiceInfo VoiceAnalyze(const float *x, float sr);

#endif
