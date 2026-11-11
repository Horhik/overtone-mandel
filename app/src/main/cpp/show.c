// Show file + video playlist. See show.h; the template below doubles as the documentation of the format.
#include "show.h"
#include "expr.h"
#include "video.h"
#include "raylib.h"   // TraceLog
#include <dirent.h>
#include <fnmatch.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const VAR_NAMES[N_SV] = { "t", "level", "db", "pulse", "beat", "f0", "ot", "prom", "chaos", "kick",
                                             "scene", "clip", "clipt" };

static const struct { const char *key; int slot, n; const char *def; } PDEF[] = {
    { "opacity",      SP_OPACITY,      1, "0.4 + 0.14*sin(t*0.052) + 0.06*sin(t*0.019 + 1.3)" },
    { "warp",         SP_WARP,         1, "0.01 + 0.025*pulse" },
    { "zoom",         SP_ZOOM,         1, "1.04 + 0.03*sin(t*0.031)" },
    { "vsat",         SP_VSAT,         1, "0.4" },
    { "vcontrast",    SP_VCONTRAST,    1, "1.3" },
    { "vtint",        SP_VTINT,        3, "0.85, 0.9, 0.85" },
    { "hue",          SP_HUE,          1, "0" },
    { "sat",          SP_SAT,          1, "1" },
    { "bright",       SP_BRIGHT,       1, "1" },
    { "contrast",     SP_CONTRAST,     1, "1" },
    { "tint",         SP_TINT,         3, "1, 1, 1" },
    { "grain_min",    SP_GRAIN_MIN,    1, "0.02" },
    { "grain_max",    SP_GRAIN_MAX,    1, "0.18" },
    { "grain_db_lo",  SP_GRAIN_DB_LO,  1, "-45" },
    { "grain_db_hi",  SP_GRAIN_DB_HI,  1, "-12" },
    { "grain_size",   SP_GRAIN_SIZE,   1, "1.5" },
    { "grain_seed",   SP_GRAIN_SEED,   1, "0" },
    { "grain_color",  SP_GRAIN_COLOR,  1, "0.3" },
    { "grain_fps",    SP_GRAIN_FPS,    1, "24" },
    { "clip",         SP_CLIP,         1, "40" },
    { "fade",         SP_FADE,         1, "6" },
    { "morph",        SP_MORPH,        1, "2" },
};
#define N_PDEF ((int)(sizeof PDEF/sizeof PDEF[0]))

static const char *TEMPLATE =
"# Сценарий видео-слоя и цвета. Файл перечитывается при сохранении, изменения перетекают за `morph` секунд.\n"
"# Строка: ключ  значение. Значение — число или формула. Нет строки — значение по умолчанию. # — комментарий.\n"
"#\n"
"# Переменные в формулах:\n"
"#   t      время, с                  level  громкость 0..1         db     громкость, дБ (-90..0)\n"
"#   pulse  громкость + доля 0..1     beat   фаза доли 0..1 (0 = удар)\n"
"#   f0     основной тон, Гц (0 — нет) ot     обертон, Гц            prom   выделенность обертона 0..1\n"
"#   chaos  хаос Physarum 0..1        kick   толчок на атаке         scene  номер сцены\n"
"#   clip   номер ролика              clipt  с от начала ролика\n"
"# Функции: sin cos tan abs min max clamp(x,a,b) mix(a,b,k) pow sqrt exp log floor fract\n"
"#          step(edge,x) smoothstep(a,b,x) noise(x) (плавный шум -1..1), константа pi, операторы + - * / % ^\n"
"\n"
"# ---- ролики: имена или шаблоны относительно этой папки, по порядку; * — все видео папки по алфавиту\n"
"clips     *\n"
"clip      40          # с на ролик (0 — не переключать)\n"
"fade      6           # с на перетекание между роликами\n"
"blend     add         # add — подмешать светом, over — полупрозрачно поверх\n"
"morph     2           # с: за сколько новые значения перетекают после сохранения файла\n"
"\n"
"# ---- видео\n"
"opacity   0.4 + 0.14*sin(t*0.052) + 0.06*sin(t*0.019 + 1.3)\n"
"warp      0.01 + 0.025*pulse       # жидкая волна\n"
"zoom      1.04 + 0.03*sin(t*0.031) # масштаб, «дыхание»\n"
"vsat      0.4                      # насыщенность видео (0 — ч/б)\n"
"vcontrast 1.3\n"
"vtint     0.85, 0.9, 0.85          # множитель R, G, B\n"
"\n"
"# ---- вся картинка (поверх этого — ползунки палитры, клавиша G)\n"
"hue       0           # поворот оттенка, градусы\n"
"sat       1           # насыщенность\n"
"bright    1           # яркость\n"
"contrast  1\n"
"tint      1, 1, 1     # множитель R, G, B\n"
"\n"
"# ---- зерно: от grain_min (тихо, db <= grain_db_lo) до grain_max (громко, db >= grain_db_hi)\n"
"grain_min    0.02\n"
"grain_max    0.18\n"
"grain_db_lo  -45\n"
"grain_db_hi  -12\n"
"grain_size   1.5      # размер зерна, пиксели экрана\n"
"grain_seed   0        # другое число — другой рисунок зерна\n"
"grain_color  0.3      # 0 — серое зерно, 1 — цветное\n"
"grain_fps    24       # сколько раз в секунду зерно меняется (0 — застыло)\n";

static char filePath[512], fileDir[512];
static Expr *cur[N_SP], *prev[N_SP];
static double reloadT = -1e9, pollT = -1e9;
static long long fileStamp = -1;
static int blendOver = 0;

#define MAX_CLIPS 128
#define MAX_SPEC  32
static char spec[MAX_SPEC][256]; static int nSpec;
static int specChanged;   // `clips` line edited: jump to the new list's first clip, report missing files
static char clips[MAX_CLIPS][512]; static int nClips;
static char forced[512];
static int vw, vh;

static Video *vCur, *vPrev;
static int curIdx = -1, swapPending, videoOn = 1;
static double clipStart, fadeStart = -1;

// ------------------------------------------------------------- parameters
static Expr *Parse(const char *src, char *err, int errLen) { return ExprParse(src, VAR_NAMES, N_SV, err, errLen); }

// split "a, b, c" at top-level commas (commas inside function calls stay)
static int SplitTop(char *s, char **parts, int max)
{
    int n = 0, depth = 0;
    parts[n++] = s;
    for (char *p = s; *p; p++) {
        if (*p == '(') depth++;
        else if (*p == ')') depth--;
        else if (*p == ',' && depth == 0 && n < max) { *p = 0; parts[n++] = p + 1; }
    }
    return n;
}

static char *Trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

static void Reload(double now, int first)
{
    Expr *next[N_SP] = { 0 };
    int keep[N_SP] = { 0 }, newBlend = 0;
    char oldSpec[MAX_SPEC][256]; int nOld = nSpec;
    memcpy(oldSpec, spec, sizeof spec);
    nSpec = 0;
    FILE *f = fopen(filePath, "r");
    char line[1024];
    int ln = 0, errors = 0;
    while (f && fgets(line, sizeof line, f)) {
        ln++;
        char *h = strchr(line, '#'); if (h) *h = 0;
        char *s = Trim(line);
        if (!*s) continue;
        char *key = s;
        while (*s && *s != ' ' && *s != '\t') s++;
        if (*s) *s++ = 0;
        char *val = Trim(s);
        if (!strcmp(key, "clips")) {
            for (char *tok = strtok(val, " \t"); tok && nSpec < MAX_SPEC; tok = strtok(NULL, " \t"))
                snprintf(spec[nSpec++], sizeof spec[0], "%s", tok);
            continue;
        }
        if (!strcmp(key, "blend")) { newBlend = !strcmp(val, "over"); continue; }
        int d = -1;
        for (int i = 0; i < N_PDEF; i++) if (!strcmp(PDEF[i].key, key)) d = i;
        if (d < 0) { TraceLog(LOG_WARNING, "SHOW: line %d: unknown key \"%s\"", ln, key); errors++; continue; }
        char *parts[3];
        int np = PDEF[d].n == 3 ? SplitTop(val, parts, 3) : (parts[0] = val, 1);
        for (int k = 0; k < PDEF[d].n; k++) {
            int slot = PDEF[d].slot + k;
            char err[128];
            const char *src = parts[k < np ? k : np - 1];   // "tint 0.9" = same for all three
            ExprFree(next[slot]);
            next[slot] = Parse(src, err, sizeof err);
            if (!next[slot]) { TraceLog(LOG_WARNING, "SHOW: line %d: %s: %s", ln, key, err); keep[slot] = 1; errors++; }
            else keep[slot] = 0;
        }
    }
    if (f) fclose(f);
    if (!f && nSpec == 0) snprintf(spec[nSpec++], sizeof spec[0], "*");
    specChanged = nSpec != nOld;
    for (int i = 0; !specChanged && i < nSpec; i++) specChanged = strcmp(spec[i], oldSpec[i]) != 0;
    for (int i = 0; i < N_PDEF; i++)
        for (int k = 0; k < PDEF[i].n; k++) {
            int slot = PDEF[i].slot + k;
            if (next[slot] || keep[slot]) continue;
            char buf[256], *parts[3], err[128];
            snprintf(buf, sizeof buf, "%s", PDEF[i].def);
            int np = PDEF[i].n == 3 ? SplitTop(buf, parts, 3) : (parts[0] = buf, 1);
            next[slot] = Parse(parts[k < np ? k : np - 1], err, sizeof err);
        }
    for (int s = 0; s < N_SP; s++) {
        if (keep[s]) continue;   // broken line: the old formula keeps running
        ExprFree(prev[s]);
        prev[s] = first ? NULL : cur[s];
        if (first) ExprFree(cur[s]);
        cur[s] = next[s];
    }
    blendOver = newBlend;
    reloadT = now;
    TraceLog(errors ? LOG_WARNING : LOG_INFO, "SHOW: %s %s%s", first ? "loaded" : "reloaded", filePath,
             errors ? TextFormat(" (%d errors, old values kept)", errors) : "");
}

void ShowEval(const float *vars, float *out, double now)
{
    float morph = cur[SP_MORPH] ? ExprEval(cur[SP_MORPH], vars) : 0;
    float k = morph > 0 ? (float)((now - reloadT)/morph) : 1;
    k = k < 0 ? 0 : k > 1 ? 1 : k;
    k = k*k*(3 - 2*k);
    for (int s = 0; s < N_SP; s++) {
        float v = cur[s] ? ExprEval(cur[s], vars) : 0;
        // timings switch at once: a morphing `clip` would fire a clip switch on the way
        int timing = s == SP_CLIP || s == SP_FADE || s == SP_MORPH;
        if (prev[s] && k < 1 && !timing) v = ExprEval(prev[s], vars) + (v - ExprEval(prev[s], vars))*k;
        out[s] = v;
    }
}

// ------------------------------------------------------------- clip list
static int IsVideo(const char *name)
{
    static const char *EXT[] = { ".mp4", ".mov", ".webm", ".mkv", ".avi", ".m4v", ".gif", ".MP4", ".MOV" };
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    for (int i = 0; i < (int)(sizeof EXT/sizeof EXT[0]); i++) if (!strcmp(dot, EXT[i])) return 1;
    return 0;
}
static int CmpStr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

// expand `spec` (names and patterns relative to the show file's folder) into `out`
static int Expand(char out[][512])
{
    int n = 0;
    if (forced[0]) { snprintf(out[n++], 512, "%s", forced); return n; }
    for (int i = 0; i < nSpec && n < MAX_CLIPS; i++) {
        const char *sp = spec[i];
        if (!strpbrk(sp, "*?[")) {
            if (sp[0] == '/') snprintf(out[n], 512, "%s", sp);
            else snprintf(out[n], 512, "%s/%s", fileDir, sp);
            if (access(out[n], R_OK) == 0) n++;
            else if (specChanged) TraceLog(LOG_WARNING, "SHOW: clips: no such file %s", out[n]);
            continue;
        }
        DIR *d = opendir(fileDir);
        if (!d) continue;
        char *names[MAX_CLIPS]; int m = 0;
        struct dirent *e;
        while ((e = readdir(d)) && m < MAX_CLIPS)
            if (e->d_name[0] != '.' && IsVideo(e->d_name) && !fnmatch(sp, e->d_name, 0)) names[m++] = strdup(e->d_name);
        closedir(d);
        qsort(names, m, sizeof names[0], CmpStr);
        for (int j = 0; j < m; j++) {
            if (n < MAX_CLIPS) snprintf(out[n++], 512, "%s/%s", fileDir, names[j]);
            free(names[j]);
        }
    }
    return n;
}

// ------------------------------------------------------------- playlist
static void StartClip(int idx, double now, int crossfade)
{
    Video *v = VideoOpen(clips[idx], vw, vh);
    if (!v) { TraceLog(LOG_WARNING, "VIDEO: cannot open %s", clips[idx]); return; }
    VideoClose(vPrev);
    if (crossfade) { vPrev = vCur; swapPending = 1; }
    else { VideoClose(vCur); vPrev = NULL; }
    vCur = v;
    curIdx = idx; clipStart = now; fadeStart = -1;
    TraceLog(LOG_INFO, "VIDEO: %d/%d %s", idx + 1, nClips, clips[idx]);
}

static void UpdateClips(double now)
{
    static char next[MAX_CLIPS][512];
    int n = Expand(next);
    int edited = specChanged;
    specChanged = 0;
    int same = n == nClips;
    for (int i = 0; same && i < n; i++) same = !strcmp(next[i], clips[i]);
    if (same && !edited) return;
    const char *was = curIdx >= 0 && curIdx < nClips ? clips[curIdx] : NULL;
    int keep = -1;
    for (int i = 0; was && i < n; i++) if (!strcmp(next[i], was)) keep = i;
    // the `clips` line was edited: go to the first clip of the new list right away;
    // only the folder changed (a file added / removed): carry on with the current clip
    if (edited && keep != 0) keep = -1;
    memcpy(clips, next, sizeof clips[0]*n);
    nClips = n;
    if (!videoOn) { curIdx = keep; return; }
    if (keep >= 0) curIdx = keep;                     // current clip still listed: carry on
    else if (n > 0) StartClip(0, now, vCur != NULL);
    else { VideoClose(vCur); VideoClose(vPrev); vCur = vPrev = NULL; curIdx = -1; }
}

ShowVideo ShowVideoStep(double now, const float *params)
{
    ShowVideo r = { 0 };
    r.over = blendOver;
    if (!videoOn || !vCur) return r;
    float clipSec = params[SP_CLIP], fadeSec = fmaxf(params[SP_FADE], 0.01f);
    if (nClips > 1 && clipSec > 0 && !vPrev && now - clipStart >= clipSec) StartClip((curIdx + 1) % nClips, now, 1);
    r.swapped = swapPending; swapPending = 0;
    r.cur = VideoFrame(vCur);
    if (r.cur && fadeStart < 0) fadeStart = now;   // fade starts with the first decoded frame of the new clip
    r.mix = 1;
    if (vPrev) {
        r.prev = VideoFrame(vPrev);
        float k = fadeStart < 0 ? 0 : (float)((now - fadeStart)/fadeSec);
        if (k >= 1) { VideoClose(vPrev); vPrev = NULL; k = 1; }
        r.mix = k*k*(3 - 2*k);
    }
    r.active = 1;
    r.name = curIdx >= 0 ? strrchr(clips[curIdx], '/') ? strrchr(clips[curIdx], '/') + 1 : clips[curIdx] : "";
    return r;
}

void ShowSkip(int dir, double now)
{
    if (!videoOn || nClips == 0) return;
    StartClip(((curIdx < 0 ? 0 : curIdx) + dir + nClips) % nClips, now, vCur != NULL);
}

void ShowSetVideo(int on, double now)
{
    videoOn = on;
    if (!on) { VideoClose(vCur); VideoClose(vPrev); vCur = vPrev = NULL; return; }
    if (nClips > 0) StartClip(curIdx >= 0 && curIdx < nClips ? curIdx : 0, now, 0);
}

int ShowVideoOn(void) { return videoOn; }
int ShowClipCount(void) { return nClips; }
int ShowClipIndex(void) { return curIdx; }
float ShowClipTime(double now) { return curIdx >= 0 ? (float)(now - clipStart) : 0; }

// ------------------------------------------------------------- file
static long long Stamp(void)
{
    struct stat st;
    if (stat(filePath, &st) != 0) return -1;
    return (long long)st.st_mtime*1000003LL + st.st_size;
}

void ShowInit(const char *path, const char *forceClip, int w, int h)   // path: show file or media folder
{
    vw = w; vh = h;
    struct stat st;
    int isDir = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
    // a folder (or a path without an extension, e.g. a media folder that doesn't exist yet) -> <folder>/show.txt
    const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    if (isDir || !strchr(base, '.')) snprintf(filePath, sizeof filePath, "%s/show.txt", path);
    else snprintf(filePath, sizeof filePath, "%s", path);
    snprintf(fileDir, sizeof fileDir, "%s", filePath);
    char *slash = strrchr(fileDir, '/');
    if (slash) *slash = 0; else snprintf(fileDir, sizeof fileDir, ".");
    if (forceClip) snprintf(forced, sizeof forced, "%s", forceClip);
#if !defined(PLATFORM_ANDROID)
    if (Stamp() < 0) {   // first run: write the commented template next to the videos
        mkdir(fileDir, 0755);
        FILE *f = fopen(filePath, "w");
        if (f) { fputs(TEMPLATE, f); fclose(f); TraceLog(LOG_INFO, "SHOW: wrote template %s", filePath); }
    }
#endif
    fileStamp = Stamp();
    Reload(0, 1);
    UpdateClips(0);   // starts the first clip
    if (nClips == 0) TraceLog(LOG_INFO, "VIDEO: no clips in %s", fileDir);
}

void ShowPoll(double now)
{
    if (now - pollT < 0.5) return;
    pollT = now;
    long long st = Stamp();
    if (st != fileStamp) { fileStamp = st; Reload(now, 0); }
    UpdateClips(now);   // also picks up videos dropped into the folder
}

void ShowShutdown(void)
{
    VideoClose(vCur); VideoClose(vPrev);
    vCur = vPrev = NULL;
    for (int s = 0; s < N_SP; s++) { ExprFree(cur[s]); ExprFree(prev[s]); cur[s] = prev[s] = NULL; }
}
