/*
 * Overtone Mandelbrot — множество Мандельброта, управляемое обертональным пением.
 *
 *   основной тон (выше)        -> скорость погружения (быстрее)
 *   частота обертона           -> направление движения (по кругу компаса)
 *   выделенность обертона      -> скорость движения
 *   тишина                     -> картинка стоит
 *
 * Тап / клик / R — сброс.  Удержание пальца / пробел — отдаление.
 * Desktop-тест без микрофона:  VOICE_WAV=file.wav ./overtone_mandel
 *
 * Глубокий зум: опорная орбита считается на CPU в double, шейдер считает
 * в float только отклонение от неё (perturbation) с перебазированием
 * (rebasing). Предел — около 1e-13 из-за double.
 */
#include "raylib.h"
#include "rlgl.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// same miniaudio configuration raylib's raudio.c is compiled with
#define MA_NO_JACK
#define MA_NO_WAV
#define MA_NO_FLAC
#define MA_NO_MP3
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MA_NO_GENERATION
#include "external/miniaudio.h"

#include "voice.h"

// ------------------------------------------------------------- tuning
#define F0_LO        80.0f     // Гц: на этой высоте погружение самое медленное
#define F0_HI       300.0f     // Гц: на этой высоте самое быстрое
#define ZOOM_MIN      0.15f    // e-кратных приближений в секунду при F0_LO
#define ZOOM_MAX      1.40f    // ... при F0_HI
#define OT_LO       500.0f     // Гц: диапазон обертона, отображаемый на полный круг
#define OT_HI      3000.0f
#define PROM_LO       8.0f     // дБ: ниже — обертона нет (обычная гласная)
#define PROM_HI      20.0f     // дБ: максимальная скорость движения
#define PAN_MAX       0.7f     // полуэкранов в секунду
#define START_X    -0.743643887037151
#define START_Y     0.131825904205330
#define START_SCALE 1.5

#if defined(PLATFORM_ANDROID)
  #define RENDER_SCALE 0.5f
  #define ITER_CAP     1500
#else
  #define RENDER_SCALE 1.0f
  #define ITER_CAP     3000
#endif

#define REF_W 1024
#define REF_H 4
#define REF_MAX (REF_W*REF_H)

// ------------------------------------------------------------- audio input
#define RING (1 << 16)
static float ring[RING];
static volatile unsigned ringW = 0;
static float sampleRate = 48000.0f;

static void PushSamples(const float *x, unsigned n)
{
    unsigned w = ringW;
    for (unsigned i = 0; i < n; i++) ring[(w + i) & (RING - 1)] = x[i];
    __atomic_store_n(&ringW, w + n, __ATOMIC_RELEASE);
}

static void CaptureCallback(ma_device *dev, void *out, const void *in, ma_uint32 frames)
{
    (void)dev; (void)out;
    if (in) PushSamples((const float *)in, frames);
}

static ma_device capDev;
static int capOn = 0;

static int StartCapture(void)
{
    ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
    cfg.capture.format = ma_format_f32;
    cfg.capture.channels = 1;
    cfg.sampleRate = 48000;
    cfg.dataCallback = CaptureCallback;
    cfg.performanceProfile = ma_performance_profile_low_latency;
    if (ma_device_init(NULL, &cfg, &capDev) != MA_SUCCESS) return 0;
    if (ma_device_start(&capDev) != MA_SUCCESS) { ma_device_uninit(&capDev); return 0; }
    sampleRate = (float)capDev.sampleRate;
    return 1;
}

// ------------------------------------------------------------- Android mic permission
#if defined(PLATFORM_ANDROID)
#include <jni.h>
#include <android_native_app_glue.h>
struct android_app *GetAndroidApp(void);

static int MicPermission(int request)
{
    struct android_app *app = GetAndroidApp();
    JavaVM *vm = app->activity->vm; JNIEnv *env;
    (*vm)->AttachCurrentThread(vm, (void *)&env, NULL);
    jobject act = app->activity->clazz;
    jclass cls = (*env)->GetObjectClass(env, act);
    jstring perm = (*env)->NewStringUTF(env, "android.permission.RECORD_AUDIO");
    jmethodID chk = (*env)->GetMethodID(env, cls, "checkSelfPermission", "(Ljava/lang/String;)I");
    int granted = (*env)->CallIntMethod(env, act, chk, perm) == 0;
    if (!granted && request) {
        jclass strCls = (*env)->FindClass(env, "java/lang/String");
        jobjectArray arr = (*env)->NewObjectArray(env, 1, strCls, perm);
        jmethodID req = (*env)->GetMethodID(env, cls, "requestPermissions", "([Ljava/lang/String;I)V");
        (*env)->CallVoidMethod(env, act, req, arr, 1);
        (*env)->DeleteLocalRef(env, arr); (*env)->DeleteLocalRef(env, strCls);
    }
    (*env)->DeleteLocalRef(env, perm); (*env)->DeleteLocalRef(env, cls);
    return granted;
}
#else
static int MicPermission(int request) { (void)request; return 1; }
#endif

// ------------------------------------------------------------- shader
static const char *FS_BODY =
"uniform vec2  uRes;\n"
"uniform float uScale;\n"
"uniform float uMaxIter;\n"
"uniform float uRefLen;\n"
"uniform float uHue;\n"
"uniform sampler2D uRef;\n"
"vec2 refZ(float n) {\n"
"    float y = floor(n / 1024.0);\n"
"    float x = n - y * 1024.0;\n"
"    return texture2D(uRef, vec2((x + 0.5) / 1024.0, (y + 0.5) / 4.0)).xy;\n"
"}\n"
"void main() {\n"
"    vec2 dc = (gl_FragCoord.xy - 0.5 * uRes) / (0.5 * uRes.y) * uScale;\n"
"    vec2 dz = vec2(0.0);\n"
"    float n = 0.0;\n"
"    vec2 z = vec2(0.0);\n"
"    float it = -1.0;\n"
"    for (int i = 0; i < 4096; i++) {\n"
"        if (float(i) >= uMaxIter) break;\n"
"        vec2 Z = refZ(n);\n"
"        dz = vec2(2.0*Z.x*dz.x - 2.0*Z.y*dz.y + dz.x*dz.x - dz.y*dz.y,\n"
"                  2.0*Z.x*dz.y + 2.0*Z.y*dz.x + 2.0*dz.x*dz.y) + dc;\n"
"        n += 1.0;\n"
"        z = refZ(n) + dz;\n"
"        float r2 = dot(z, z);\n"
"        if (r2 > 1.0e4) { it = float(i); break; }\n"
"        if (r2 < dot(dz, dz) || n >= uRefLen - 1.0) { dz = z; n = 0.0; }\n"
"    }\n"
"    if (it < 0.0) { gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }\n"
"    float mu = it + 1.0 - log2(log(dot(z, z)) * 0.5);\n"
"    float t = sqrt(mu) * 0.11 + uHue;\n"
"    vec3 col = 0.5 + 0.5 * cos(6.2831853 * (t + vec3(0.0, 0.18, 0.42)));\n"
"    gl_FragColor = vec4(col, 1.0);\n"
"}\n";

static Shader LoadMandelShader(void)
{
    const char *hdr;
    switch (rlGetVersion()) {
    case RL_OPENGL_ES_20:
        hdr = "#version 100\nprecision highp float;\nprecision highp sampler2D;\nvarying vec2 fragTexCoord;\n"; break;
    case RL_OPENGL_ES_30:
        hdr = "#version 300 es\nprecision highp float;\nprecision highp sampler2D;\nin vec2 fragTexCoord;\n"
              "out vec4 finalColor;\n#define gl_FragColor finalColor\n#define texture2D texture\n"; break;
    default:
        hdr = "#version 330\nin vec2 fragTexCoord;\nout vec4 finalColor;\n"
              "#define gl_FragColor finalColor\n#define texture2D texture\n"; break;
    }
    size_t n = strlen(hdr) + strlen(FS_BODY) + 1;
    char *src = malloc(n);
    strcpy(src, hdr); strcat(src, FS_BODY);
    Shader s = LoadShaderFromMemory(NULL, src);
    free(src);
    return s;
}

// reference orbit at (cx, cy) in double -> float texture
static float refBuf[REF_MAX*4];
static int ComputeRef(double cx, double cy, int maxIter)
{
    double x = 0, y = 0;
    int n = 0;
    memset(refBuf, 0, sizeof refBuf);
    for (; n < maxIter && n < REF_MAX; n++) {
        refBuf[n*4] = (float)x; refBuf[n*4 + 1] = (float)y;
        if (x*x + y*y > 1e4) { n++; break; }
        double t = x*x - y*y + cx; y = 2*x*y + cy; x = t;
    }
    return n;
}

// ------------------------------------------------------------- helpers
static float Clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static float Median5(float *h, float v)
{
    for (int i = 0; i < 4; i++) h[i] = h[i + 1];
    h[4] = v;
    float s[5]; memcpy(s, h, sizeof s);
    for (int i = 1; i < 5; i++) for (int j = i; j > 0 && s[j] < s[j-1]; j--) { float t = s[j]; s[j] = s[j-1]; s[j-1] = t; }
    return s[2];
}

int main(void)
{
    SetConfigFlags(FLAG_VSYNC_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(1280, 720, "Overtone Mandelbrot");
    SetTargetFPS(60);

    // optional file input instead of the microphone (desktop testing)
    Wave fileWave = { 0 }; float *fileData = NULL; double filePos = 0;
    const char *wavPath = getenv("VOICE_WAV");
    if (wavPath) {
        fileWave = LoadWave(wavPath);
        if (fileWave.frameCount) {
            WaveFormat(&fileWave, fileWave.sampleRate, 32, 1);
            fileData = (float *)fileWave.data;
            sampleRate = (float)fileWave.sampleRate;
        }
    }
    const char *shotDir = getenv("SHOT_DIR");      // test: periodic screenshots
    double shotNext = 0.5;

    Shader sh = LoadMandelShader();
    int locRes = GetShaderLocation(sh, "uRes"), locScale = GetShaderLocation(sh, "uScale");
    int locIter = GetShaderLocation(sh, "uMaxIter"), locLen = GetShaderLocation(sh, "uRefLen");
    int locHue = GetShaderLocation(sh, "uHue"), locRef = GetShaderLocation(sh, "uRef");

    Image refImg = { refBuf, REF_W, REF_H, 1, PIXELFORMAT_UNCOMPRESSED_R32G32B32A32 };
    Texture2D refTex = LoadTextureFromImage(refImg);
    SetTextureFilter(refTex, TEXTURE_FILTER_POINT);

    RenderTexture2D rt = { 0 };
    int rtW = 0, rtH = 0;

    double cx = START_X, cy = START_Y, scale = START_SCALE;
    if (getenv("TEST_SCALE")) scale = atof(getenv("TEST_SCALE"));   // debug hook
    float hist[5] = { 0 }, f0s = 0, otS = 0, promS = 0, level = 0;
    float zoomRate = 0, vx = 0, vy = 0, otAngle = 0;
    int voiced = 0, analysisTick = 0;
    double permTimer = 0; int asked = 0;
    float window[VOICE_N];
    double startTime = GetTime();

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        if (dt > 0.1f) dt = 0.1f;

        // ---- audio source
        if (fileData) {
            double want = (GetTime() - startTime)*sampleRate;
            unsigned n = 0;
            while (filePos < want && filePos < fileWave.frameCount) { filePos++; n++; }
            unsigned off = (unsigned)filePos - n;
            if (n) PushSamples(fileData + off, n);
            if (filePos >= fileWave.frameCount) { float z[1024] = {0}; PushSamples(z, 1024); }
        } else if (!capOn) {
            permTimer -= dt;
            if (permTimer <= 0) {
                permTimer = 1.0;
                if (MicPermission(!asked)) capOn = StartCapture();
                asked = 1;
            }
        }

        // ---- voice analysis (every other frame)
        if (++analysisTick % 2 == 0) {
            unsigned w = __atomic_load_n(&ringW, __ATOMIC_ACQUIRE);
            if (w >= VOICE_N) {
                for (int i = 0; i < VOICE_N; i++) window[i] = ring[(w - VOICE_N + i) & (RING - 1)];
                VoiceInfo v = VoiceAnalyze(window, sampleRate);
                level = v.rms;
                voiced = v.voiced && v.rms > 0.003f;
                if (voiced) {
                    float f = Median5(hist, v.f0);
                    f0s = f0s > 0 ? f0s + (f - f0s)*0.35f : f;
                    float p = Clamp01((v.otProm - PROM_LO)/(PROM_HI - PROM_LO));
                    promS += (p - promS)*0.3f;
                    if (p > 0 && v.otFreq > 0) otS = otS > 0 ? otS + (v.otFreq - otS)*0.35f : v.otFreq;
                } else {
                    promS *= 0.7f;
                }
            }
        }

        // ---- controls from voice
        float targetZoom = 0;
        if (voiced && f0s > 0) {
            float u = Clamp01(logf(f0s/F0_LO)/logf(F0_HI/F0_LO));
            targetZoom = ZOOM_MIN + (ZOOM_MAX - ZOOM_MIN)*u;
        }
        if (IsKeyDown(KEY_SPACE) || (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && GetGestureHoldDuration() > 0.35f))
            targetZoom = -1.0f;
        zoomRate += (targetZoom - zoomRate)*(1 - expf(-dt*6));

        float tvx = 0, tvy = 0;
        if (otS > 0) {
            float u = logf(otS/OT_LO)/logf(OT_HI/OT_LO);
            otAngle = 6.2831853f*u + 1.5707963f;       // 500 Hz -> вверх, далее против часовой
            tvx = cosf(otAngle)*promS*PAN_MAX;
            tvy = sinf(otAngle)*promS*PAN_MAX;
        }
        vx += (tvx - vx)*(1 - expf(-dt*5));
        vy += (tvy - vy)*(1 - expf(-dt*5));

        cx += vx*scale*dt;
        cy += vy*scale*dt;
        double minScale = 2e-13*fmax(1.0, sqrt(cx*cx + cy*cy));
        scale *= exp(-zoomRate*dt);
        int atLimit = scale <= minScale;
        if (atLimit) scale = minScale;
        if (scale > 4) scale = 4;

        if (IsKeyPressed(KEY_R) || IsGestureDetected(GESTURE_DOUBLETAP)) {
            cx = START_X; cy = START_Y; scale = START_SCALE;
        }

        // ---- render
        int sw = GetScreenWidth(), shh = GetScreenHeight();
        int w = (int)(sw*RENDER_SCALE), h = (int)(shh*RENDER_SCALE);
        if (w != rtW || h != rtH) {
            if (rtW) UnloadRenderTexture(rt);
            rt = LoadRenderTexture(w, h); rtW = w; rtH = h;
            SetTextureFilter(rt.texture, TEXTURE_FILTER_BILINEAR);
        }

        float depth = (float)log2(START_SCALE/scale);
        int maxIter = 250 + (int)(depth*60);
        if (maxIter > ITER_CAP) maxIter = ITER_CAP;
        int refLen = ComputeRef(cx, cy, maxIter);
        UpdateTexture(refTex, refBuf);

        float res[2] = { (float)w, (float)h }, fs = (float)scale, fi = (float)maxIter, fl = (float)refLen;
        float hue = (float)(depth*0.02 + GetTime()*0.01);
        BeginTextureMode(rt);
            BeginShaderMode(sh);
                SetShaderValue(sh, locRes, res, SHADER_UNIFORM_VEC2);
                SetShaderValue(sh, locScale, &fs, SHADER_UNIFORM_FLOAT);
                SetShaderValue(sh, locIter, &fi, SHADER_UNIFORM_FLOAT);
                SetShaderValue(sh, locLen, &fl, SHADER_UNIFORM_FLOAT);
                SetShaderValue(sh, locHue, &hue, SHADER_UNIFORM_FLOAT);
                SetShaderValueTexture(sh, locRef, refTex);
                DrawRectangle(0, 0, w, h, WHITE);
            EndShaderMode();
        EndTextureMode();

        BeginDrawing();
            ClearBackground(BLACK);
            DrawTexturePro(rt.texture, (Rectangle){ 0, 0, (float)w, (float)-h },
                           (Rectangle){ 0, 0, (float)sw, (float)shh }, (Vector2){ 0, 0 }, 0, WHITE);

            // HUD
            int fsz = shh/30 > 14 ? shh/30 : 14;
            int x0 = fsz, y0 = fsz;
            DrawRectangle(x0 - fsz/2, y0 - fsz/2, fsz*16, fsz*7, Fade(BLACK, 0.45f));
            DrawText(TextFormat("zoom  1e%.1f%s", depth*0.30103f, atLimit ? "  (limit)" : ""), x0, y0, fsz, RAYWHITE);
            DrawText(voiced ? TextFormat("f0    %.0f Hz", f0s) : "f0    --", x0, y0 + fsz*1.3f, fsz, RAYWHITE);
            DrawText(promS > 0.02f ? TextFormat("over  %.0f Hz", otS) : "over  --", x0, y0 + fsz*2.6f, fsz, RAYWHITE);
            DrawRectangle(x0, y0 + fsz*4.0f, (int)(fsz*8*Clamp01(level*8)), fsz/2, GREEN);
            DrawRectangle(x0, y0 + fsz*4.8f, (int)(fsz*8*promS), fsz/2, ORANGE);
            if (!fileData && !capOn) DrawText("waiting for microphone...", x0, y0 + fsz*5.6f, fsz, YELLOW);

            // compass: where the overtone points
            Vector2 c = { (float)(sw - fsz*4), (float)(fsz*4) };
            float r = fsz*2.6f;
            DrawCircleLinesV(c, r, Fade(RAYWHITE, 0.6f));
            DrawLineEx(c, (Vector2){ c.x + cosf(otAngle)*r, c.y - sinf(otAngle)*r }, 3, Fade(ORANGE, 0.3f + 0.7f*promS));
            DrawCircleV(c, 3, RAYWHITE);
        EndDrawing();

        if (shotDir && GetTime() - startTime > shotNext) {
            TakeScreenshot(TextFormat("%s/shot_%05.1f.png", shotDir, shotNext));
            TraceLog(LOG_INFO, "SHOT t=%.1f f0=%.0f ot=%.0f prom=%.2f zoom=%.2f scale=%.3e c=(%.12f,%.12f)",
                     shotNext, f0s, otS, promS, zoomRate, scale, cx, cy);
            shotNext += 0.5;
            if (fileData && filePos >= fileWave.frameCount && shotNext > fileWave.frameCount/sampleRate + 1.5) break;
        }
    }

    if (capOn) ma_device_uninit(&capDev);
    if (fileWave.data) UnloadWave(fileWave);
    UnloadRenderTexture(rt);
    UnloadTexture(refTex);
    UnloadShader(sh);
    CloseWindow();
    return 0;
}
