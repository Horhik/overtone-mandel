// Tempo (BPM) and beat phase tracking for music: onset envelope -> autocorrelation -> comb phase
#ifndef BEAT_H
#define BEAT_H

typedef struct {
    float bpm;      // current tempo estimate, 0 until known
    float conf;     // 0..1, how periodic the onsets are
    float phase;    // 0..1 position inside the current beat (0 = on the beat)
} BeatInfo;

void     BeatInit(float sampleRate);
void     BeatPush(const float *x, int n);   // mono samples, any chunk size
BeatInfo BeatGet(void);

#endif
