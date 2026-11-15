// Show file: a text file with one parameter per line ("key  value-or-formula"), watched while running.
// On save every parameter morphs from the old formula to the new one over `morph` seconds.
// Also runs the video playlist (clips from the file, crossfades between them).
#ifndef SHOW_H
#define SHOW_H

// variables usable in formulas, filled by the caller every frame
enum { SV_T, SV_LEVEL, SV_DB, SV_PULSE, SV_BEAT, SV_F0, SV_OT, SV_PROM, SV_CHAOS, SV_KICK, SV_SCENE,
       SV_CLIP, SV_CLIPT, N_SV };

// parameter slots; three-component ones (tints) take 3 consecutive slots
enum {
    SP_OPACITY, SP_WARP, SP_ZOOM, SP_VHUE, SP_VSAT, SP_VBRIGHT, SP_VCONTRAST, SP_VTINT, SP_VTINT_G, SP_VTINT_B,
    SP_ZHUE, SP_ZSAT, SP_ZBRIGHT, SP_ZCONTRAST, SP_ZTINT, SP_ZTINT_G, SP_ZTINT_B,
    SP_HUE, SP_SAT, SP_BRIGHT, SP_CONTRAST, SP_TINT, SP_TINT_G, SP_TINT_B,
    SP_GRAIN_MIN, SP_GRAIN_MAX, SP_GRAIN_DB_LO, SP_GRAIN_DB_HI, SP_GRAIN_SIZE, SP_GRAIN_SEED, SP_GRAIN_COLOR, SP_GRAIN_FPS,
    SP_CLIP, SP_FADE, SP_MORPH,
    N_SP
};

typedef struct {
    const unsigned char *cur, *prev;   // new frames to upload this frame (NULL = keep the texture)
    int swapped;                       // a new clip started: the caller swaps its cur/prev textures first
    float mix;                         // 0 = only prev, 1 = only cur
    int active;                        // anything to draw
    int over;                          // blend: 0 = add, 1 = alpha over
    const char *name;                  // current clip file name
} ShowVideo;

// path: the show file, or a media folder (then <folder>/show.txt); forceClip: VIDEO=..., overrides `clips`
void ShowInit(const char *path, const char *forceClip, int vw, int vh);
void ShowPoll(double now);                        // re-reads the file when it changed (checks twice a second)
void ShowEval(const float *vars, float *out, double now);   // out[N_SP]
ShowVideo ShowVideoStep(double now, const float *params);
void ShowSkip(int dir, double now);               // next / previous clip
void ShowSetVideo(int on, double now);
int  ShowVideoOn(void);
int  ShowClipCount(void);
int  ShowClipIndex(void);                         // 0-based, -1 if none
float ShowClipTime(double now);                   // seconds since the current clip started
const char *ShowDir(void);                        // folder of the show file
void ShowShutdown(void);

#endif
