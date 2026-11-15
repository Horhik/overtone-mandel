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
#include "beat.h"
#include "physarum.h"
#include "show.h"
#include "remote.h"

// ------------------------------------------------------------- tuning
#define F0_LO        70.0f     // Гц: диапазон ползунка базового тона
#define F0_HI       400.0f
#define F0_BASE     100.0f     // Гц: базовый тон по умолчанию (выше — приближение, ниже — отдаление)
#define DEAD_SEMI     0.5f     // полутонов вокруг базы, где зум стоит
#define FULL_SEMI     7.0f     // полутонов от базы до полной скорости
#define ZOOM_MAX      1.40f    // e-кратных приближений в секунду
#define ZOOM_OUT_MAX  1.00f    // ... отдалений в секунду
#define OT_LO       500.0f     // Гц: диапазон обертона, отображаемый на полный круг
#define OT_HI      3000.0f
#define PROM_LO       8.0f     // дБ: ниже — обертона нет (обычная гласная)
#define PROM_HI      20.0f     // дБ: максимальная скорость движения
#define PAN_MAX       0.7f     // полуэкранов в секунду
#define OT_SWITCH     0.2      // с: столько новый обертон должен держаться, чтобы сменить захваченный
#define DIR_RATE      2.0f     // 1/с: скорость поворота направления при уверенном обертоне
#define VEL_RATE      3.0f     // 1/с: сглаживание скорости движения
#define START_X    -0.743643887037151
#define START_Y     0.131825904205330
#define START_SCALE 1.5
// Точки погружения: точки Мисюревича (f^(k+p)(c) = f^k(c), уточнены Ньютоном в 50 знаков). Лежат на границе
// множества, узор вокруг самоподобен на любой глубине — в чёрное не провалиться. Цикл: дошли до предела
// точности -> пение только отдаляет -> по пути плавно переезжаем к следующей точке -> снова погружение.
static const struct { double x, y; const char *name; } DIVES[] = {
    { -0.776610592599701857,  0.134608961675028166, "seahorse spirals" },   // M(23,2)
    {  0.284884537273174365, -0.011121821889581933, "elephant valley"  },   // M(17,1)
    { -0.101096363845622164, -0.956286510809141510, "quad spiral"      },   // M(4,1)
    { -0.774680813028235771, -0.137416807154791570, "scepter"          },   // M(28,1)
    {  0.0,                   1.0,                  "dendrite (c = i)" },   // M(1,2)
};
#define N_DIVES ((int)(sizeof DIVES/sizeof DIVES[0]))

// Scenes. M steps through all of them, A = auto show: every SCENE_SEC a cross-fade (FADE_SEC) to the next
// one in SHOW. SHOW never puts two Mandelbrot scenes next to each other (they share one reference orbit).
// The two Mandelbrot scenes are too heavy for a laptop without a GPU: off unless MANDEL=1.
enum { MODE_DRIFT, MODE_DIVE, MODE_ORG, MODE_NEWTON, MODE_APOLLO };
static const struct { int mode, preset; const char *name; } SCENES[] = {
    { MODE_DRIFT,  0, "drift"              },
    { MODE_DIVE,   0, "dive + colour"      },
    { MODE_ORG,    0, "organism: mandala"  },
    { MODE_ORG,    1, "organism: cells"    },
    { MODE_ORG,    2, "organism: vessels"  },
    { MODE_NEWTON, 0, "newton"             },
    { MODE_APOLLO, 0, "apollonian"         },
};
#define N_SCENES ((int)(sizeof SCENES/sizeof SCENES[0]))
static const int SHOW[] = { 1, 2, 5, 3, 6, 4 };
static int mandelOn;
static int SceneOn(int i) { return mandelOn || SCENES[i].mode >= MODE_ORG; }
#define N_SHOW ((int)(sizeof SHOW/sizeof SHOW[0]))
static const struct { float c[3], zoom; } ORG_PRESETS[] = {
    { { -0.45f, -0.55f, -0.85f }, 1.0f },   // mandalas, rings, cells
    { { -0.60f, -0.80f, -0.50f }, 1.0f },   // colonies of round cells
    { { -0.90f, -0.90f, -0.90f }, 1.2f },   // rings and vessels
};
#define SCENE_SEC    75.0f    // с на сцену в авто-шоу
#define FADE_SEC      6.0f    // с на перетекание
// Режим «organism» (срез 3D Kaliset)
#define ORG_SLICE0   0.35f    // стартовая координата среза
#define SLICE_MAX    0.12f    // единиц третьего измерения в секунду при полной скорости
#define MUT_SPEED    0.06f    // скорость мутации C от обертона
#define BEAT_AMT     0.6f     // сила пульсации в долю по умолчанию (0..1.5)
#define VID_W        640      // видео-слой декодируется в этом размере (растягивается на экран)
#define VID_H        360
#define PHYS_W       480      // сетка Physarum (растягивается на экран)
#define PHYS_H       270
#define PHYS_AGENTS  60000    // ~2 мс/шаг на одном ядре
#define CHAOS_NOTE_LO 36      // C2 (65 Гц): самая частая нота ниже — плесень спокойная и тяжёлая
#define CHAOS_NOTE_HI 96      // C7 (2093 Гц): выше — максимум хаоса
#define CHAOS_AMT     1.0f    // общая сила хаоса
#define MUT_R        0.22f    // амплитуда мутации C вокруг базового (качается по синусу, не застревает)
#define SLICE_C      0.15f    // срез качается вокруг этой глубины...
#define SLICE_A      1.0f     // ...с этой амплитудой: дальше ~±1.5 узор пустеет (проверено рендером)
#define LIMIT_HOLD    1.0f     // с на пределе, прежде чем начнётся всплытие
#define ASCENT_MIN    0.8f     // e-кратных отдалений в секунду при любом пении во время всплытия
#define ASCENT_MAX    2.2f     // ... чем дальше от базового тона, тем быстрее
#define ASCENT_TOP    0.03     // масштаб, выше которого (и уже над новой точкой) снова можно погружаться
#define ASCENT_PAN    0.6      // полуэкранов в секунду: переезд к новой точке

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
    if (ma_device_init(NULL, &cfg, &capDev) != MA_SUCCESS) {
        TraceLog(LOG_WARNING, "MIC: no capture device (audio backend libs not found? on NixOS run inside `nix develop .#desktop`)");
        return 0;
    }
    if (ma_device_start(&capDev) != MA_SUCCESS) { TraceLog(LOG_WARNING, "MIC: capture device failed to start"); ma_device_uninit(&capDev); return 0; }
    TraceLog(LOG_INFO, "MIC: %s, %u Hz, backend %s", capDev.capture.name, capDev.sampleRate, ma_get_backend_name(capDev.pContext->backend));
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
"uniform float uPulse;\n"   // loudness 0..1: breathing of the glowing threads
"uniform float uGlow;\n"    // overtone strength 0..1: lichen -> embers
"uniform sampler2D uRef;\n"
"vec2 refZ(float n) {\n"
"    float y = floor(n / 1024.0);\n"
"    float x = n - y * 1024.0;\n"
"    return texture2D(uRef, vec2((x + 0.5) / 1024.0, (y + 0.5) / 4.0)).xy;\n"
"}\n"
"void main() {\n"
"    vec2 dc = (gl_FragCoord.xy - 0.5 * uRes) / (0.5 * uRes.y) * uScale;\n"
"    vec2 dz = vec2(0.0);\n"
"    float px = uScale / (0.5 * uRes.y);\n"
"    vec2 der = vec2(0.0);\n"   // dz/dc in pixels, for the distance to the boundary
"    float n = 0.0;\n"
"    vec2 z = refZ(0.0);\n"
"    float it = -1.0;\n"
"    for (int i = 0; i < 4096; i++) {\n"
"        if (float(i) >= uMaxIter) break;\n"
"        vec2 Z = refZ(n);\n"
"        dz = vec2(2.0*Z.x*dz.x - 2.0*Z.y*dz.y + dz.x*dz.x - dz.y*dz.y,\n"
"                  2.0*Z.x*dz.y + 2.0*Z.y*dz.x + 2.0*dz.x*dz.y) + dc;\n"
"        n += 1.0;\n"
"        der = 2.0 * vec2(z.x*der.x - z.y*der.y, z.x*der.y + z.y*der.x) + vec2(px, 0.0);\n"
"        z = refZ(n) + dz;\n"
"        float r2 = dot(z, z);\n"
"        if (r2 > 1.0e4) { it = float(i); break; }\n"
"        if (r2 < dot(dz, dz) || n >= uRefLen - 1.0) { dz = z; n = 0.0; }\n"
"    }\n"
"    vec2 uv = (gl_FragCoord.xy - 0.5 * uRes) / uRes.y;\n"
"    float vig = 1.0 - 0.55 * dot(uv, uv);\n"
"    if (it < 0.0) { gl_FragColor = vec4(vec3(0.004, 0.016, 0.009) * (1.0 + 0.8 * uPulse) * vig, 1.0); return; }\n"
"    float r = length(z);\n"
"    float mu = it + 1.0 - log2(log(r));\n"
"    float t = sqrt(mu) * 0.08 + uHue;\n"
     // dark forest: black-green, pine, moss, deep teal
"    vec3 base = vec3(0.055, 0.12, 0.075) + vec3(0.05, 0.10, 0.065) * cos(6.2831853 * (t + vec3(0.0, 0.10, 0.27)));\n"
     // glowing threads along the boundary (distance estimate, in pixels)
"    float dpx = r * log(r) / max(length(der), 1e-20);\n"
"    float thread = exp(-dpx * (1.4 - 0.8 * uPulse));\n"
"    vec3 lichen = vec3(0.26, 0.60, 0.26), ember = vec3(0.80, 0.40, 0.08);\n"
"    vec3 glow = mix(lichen, ember, 0.6 * uGlow) * thread * (0.35 + 1.1 * uPulse);\n"
"    vec3 col = base * (0.55 + 0.6 * uPulse) + glow;\n"
"    gl_FragColor = vec4(col * vig, 1.0);\n"
"}\n";

// "organism": 2D slice of the 3D Kaliset (Pablo Roman Andrioli): p = |p|/(p.p) + C.
// Moving the slice through the third dimension and nudging C makes the structure flow and mutate.
static const char *FS_ORG =
"uniform vec2  uRes;\n"
"uniform float uTime;\n"     // flow time: runs faster when it's loud
"uniform vec2  uRot;\n"      // 3D orientation of the slice plane
"uniform float uSlice;\n"
"uniform vec3  uC;\n"
"uniform float uZoom;\n"
"uniform float uHue;\n"
"uniform float uPulse;\n"
"uniform float uGlow;\n"
"mat2 rot(float a) { float c = cos(a), s = sin(a); return mat2(c, s, -s, c); }\n"
"void main() {\n"
"    vec2 uv = (gl_FragCoord.xy - 0.5 * uRes) / uRes.y;\n"
"    vec2 q = uv * uZoom;\n"
"    q += 0.035 * (1.0 + 1.5 * uPulse) * vec2(sin(q.y * 3.1 + uTime * 0.37), sin(q.x * 2.7 - uTime * 0.29));\n"   // slow liquid warp
"    vec3 p = vec3(q, uSlice);\n"
"    p.xz = rot(uRot.x) * p.xz;\n"
"    p.yz = rot(uRot.y) * p.yz;\n"
"    float pa = 0.0, acc = 0.0, vein = 0.0, trap = 1e3, w = 1.0, ws = 0.0;\n"
"    for (int i = 0; i < ORG_ITER; i++) {\n"
"        p = abs(p) / dot(p, p) + uC;\n"
"        float l = length(p);\n"
"        float d = abs(l - pa);\n"
"        acc += d * w;\n"                 // deep iterations are chaotic: weigh them down (less grain)
"        vein += exp(-12.0 * d) * w;\n"
"        ws += w;\n"
"        trap = min(trap, abs(p.z) + 0.15 * float(i) / float(ORG_ITER));\n"
"        w *= 0.82;\n"
"        pa = l;\n"
"    }\n"
"    float tn = acc / ws * 0.55;\n"
"    float tissue = clamp(tn, 0.0, 1.5);\n"
     // huge accumulations (orbit thrown far) become dark cell bodies with a lit edge, not flat blobs
"    float band = smoothstep(0.15, 0.7, tn) * (1.0 - 0.85 * smoothstep(0.85, 1.5, tn));\n"
"    float v = vein / ws;\n"
"    float membrane = exp(-trap * 9.0);\n"
"    float t = tissue * 0.6 + uHue;\n"
"    vec3 flesh = vec3(0.05, 0.11, 0.06) + vec3(0.045, 0.09, 0.05) * cos(6.2831853 * (t + vec3(0.0, 0.08, 0.25)));\n"
"    vec3 bone = vec3(0.62, 0.64, 0.50), ember = vec3(0.85, 0.36, 0.06), lichen = vec3(0.25, 0.58, 0.24);\n"
"    vec3 col = flesh * (0.35 + 1.5 * band) * (0.6 + 0.6 * uPulse);\n"
"    col *= 1.0 - 0.5 * membrane * membrane;\n"
"    col += mix(lichen, bone, 0.65) * 4.0 * membrane * (1.0 - membrane) * (0.10 + 0.40 * uPulse);\n"
"    float rim = pow(4.0 * v * (1.0 - v), 1.5);\n"                // glowing membrane at the edge of a cell
"    float core = smoothstep(0.55, 0.95, v);\n"                    // converged inside: a dark nucleus
"    col *= 1.0 - 0.85 * core;\n"
"    col += vec3(0.02, 0.05, 0.03) * core * (1.0 + 2.0 * uPulse);\n"
"    col += mix(lichen, ember, uGlow) * rim * (0.35 + 1.2 * uPulse);\n"
"    float vig = 1.0 - 0.6 * dot(uv, uv);\n"
"    gl_FragColor = vec4(col * vig, 1.0);\n"
"}\n";

// Shared uniforms of the "living" scenes (organism / newton / apollonian): same controls for all of them.
#define LIVE_UNIFORMS \
"uniform vec2  uRes;\n" "uniform float uTime;\n" "uniform vec2 uRot;\n" "uniform float uSlice;\n" \
"uniform vec3  uC;\n" "uniform float uZoom;\n" "uniform float uHue;\n" "uniform float uPulse;\n" "uniform float uGlow;\n" \
"mat2 rot(float a) { float c = cos(a), s = sin(a); return mat2(c, s, -s, c); }\n" \
"vec2 cmul(vec2 a, vec2 b) { return vec2(a.x*b.x - a.y*b.y, a.x*b.y + a.y*b.x); }\n" \
"vec2 cdiv(vec2 a, vec2 b) { return vec2(a.x*b.x + a.y*b.y, a.y*b.x - a.x*b.y) / max(dot(b, b), 1e-12); }\n" \
"vec2 cs(float a) { return vec2(cos(a), sin(a)); }\n"

// "newton": basins of relaxed Newton's method for a cubic whose roots orbit and breathe.
// uSlice = relaxation (past 1 the boundaries turn chaotic), uC.xy (overtone mutation) drags one root.
static const char *FS_NEWTON = LIVE_UNIFORMS
"void main() {\n"
"    vec2 uv = (gl_FragCoord.xy - 0.5 * uRes) / uRes.y;\n"
"    vec2 z = rot(uRot.x * 0.5) * uv * uZoom * 1.5;\n"
"    z += 0.03 * (1.0 + uPulse) * vec2(sin(z.y * 2.3 + uTime * 0.3), sin(z.x * 2.1 - uTime * 0.27));\n"
"    float t = uTime * 0.04;\n"
"    vec2 r0 = cs(t) * (1.0 + 0.15 * sin(uTime * 0.13));\n"
"    vec2 r1 = cs(t + 2.0944) * (1.0 + 0.15 * sin(uTime * 0.11 + 1.0)) + uC.xy * 2.5;\n"
"    vec2 r2 = cs(t + 4.1888) * (1.0 + 0.15 * sin(uTime * 0.09 + 2.0));\n"
"    float a = clamp(1.0 + 0.55 * uSlice, 0.55, 1.75);\n"
"    float it = 40.0; int root = -1;\n"
"    for (int i = 0; i < 40; i++) {\n"
"        vec2 d0 = z - r0, d1 = z - r1, d2 = z - r2;\n"
"        vec2 p = cmul(cmul(d0, d1), d2);\n"
"        vec2 dp = cmul(d1, d2) + cmul(d0, d2) + cmul(d0, d1);\n"
"        z -= a * cdiv(p, dp);\n"
"        if (dot(z - r0, z - r0) < 1e-4) { root = 0; it = float(i); break; }\n"
"        if (dot(z - r1, z - r1) < 1e-4) { root = 1; it = float(i); break; }\n"
"        if (dot(z - r2, z - r2) < 1e-4) { root = 2; it = float(i); break; }\n"
"    }\n"
"    float ph = uHue * 6.2831853;\n"
"    vec3 c0 = vec3(0.10, 0.22, 0.09), c1 = vec3(0.03, 0.14, 0.13), c2 = vec3(0.17, 0.17, 0.10);\n"
"    vec3 basin = root == 0 ? c0 : root == 1 ? c1 : root == 2 ? c2 : vec3(0.0);\n"
"    basin = mix(basin, basin.gbr, 0.5 + 0.5 * sin(ph));\n"
"    float bands = 0.5 + 0.5 * cos(it * 0.9 + uTime * 0.2);\n"     // petals: rings of equal convergence time
"    float near = exp(-it * 0.12);\n"
"    vec3 col = basin * (0.4 + 2.2 * near * (0.55 + 0.45 * bands)) * (0.7 + 0.5 * uPulse);\n"
"    float edge = smoothstep(4.0, 14.0, it);\n"                    // slow convergence = basin boundary: glow
"    vec3 lichen = vec3(0.26, 0.60, 0.26), ember = vec3(0.80, 0.40, 0.08);\n"
"    col += mix(lichen, ember, uGlow) * edge * (0.25 + 0.9 * uPulse);\n"
"    float vig = 1.0 - 0.6 * dot(uv, uv);\n"
"    gl_FragColor = vec4(col * vig, 1.0);\n"
"}\n";

// "apollonian": circle-inversion fold (2D Apollonian gasket family): nested rings, sacred geometry.
// uSlice changes the inversion scale (the whole packing re-forms), uC.xy (mutation) shifts the fold.
static const char *FS_APOLLO = LIVE_UNIFORMS
"void main() {\n"
"    vec2 uv = (gl_FragCoord.xy - 0.5 * uRes) / uRes.y;\n"
"    float zoom = uZoom * 1.3;\n"
"    vec2 z = rot(uRot.x * 0.3) * uv * zoom + vec2(0.25 * sin(uTime * 0.031), 0.25 * cos(uTime * 0.023));\n"
"    z += 0.02 * (1.0 + uPulse) * vec2(sin(z.y * 3.0 + uTime * 0.4), sin(z.x * 2.6 - uTime * 0.33));\n"
"    float s = 1.18 + 0.14 * sin(uSlice * 1.7) + 0.25 * uC.x;\n"
"    vec2 sh = uC.yz * 0.3;\n"
"    float sc = 1.0; vec4 orb = vec4(1e3);\n"
"    for (int i = 0; i < 10; i++) {\n"
"        z = -1.0 + 2.0 * fract(0.5 * z + 0.5 + sh);\n"
"        float r2 = dot(z, z);\n"
"        orb = min(orb, vec4(abs(z), r2, abs(r2 - 0.5)));\n"
"        float k = s / r2;\n"
"        z *= k; sc *= k;\n"
"    }\n"
"    float d = 0.25 * abs(z.y) / sc;\n"
"    float px = zoom / uRes.y;\n"
"    float line = exp(-d / px * (3.0 - 1.5 * uPulse));\n"
"    float t = sqrt(orb.z) * 0.9 + uHue;\n"
"    vec3 base = vec3(0.05, 0.11, 0.07) + vec3(0.045, 0.09, 0.06) * cos(6.2831853 * (t + vec3(0.0, 0.1, 0.27)));\n"
"    vec3 col = base * (0.5 + 0.8 * exp(-orb.w * 4.0)) * (0.7 + 0.5 * uPulse);\n"
"    vec3 bone = vec3(0.62, 0.64, 0.50), lichen = vec3(0.26, 0.60, 0.26), ember = vec3(0.80, 0.40, 0.08);\n"
"    col += mix(mix(lichen, bone, 0.4), ember, uGlow) * line * (0.25 + 0.7 * uPulse);\n"
"    float vig = 1.0 - 0.6 * dot(uv, uv);\n"
"    gl_FragColor = vec4(col * vig, 1.0);\n"
"}\n";

// video layer, drawn over the scene (additive or alpha): crossfade of two clips, breathing scale,
// liquid warp, desaturate + contrast + tint. Alpha = opacity. Everything comes from the show file.
static const char *FS_FILM =
"uniform sampler2D texture0;\n"   // current clip
"uniform sampler2D uPrev;\n"      // previous clip while crossfading
"uniform float uMix;\n"
"uniform float uOp;\n"
"uniform float uTime;\n"
"uniform float uWarp;\n"
"uniform float uZoom;\n"
"uniform float uSat;\n"
"uniform float uContrast;\n"
"uniform vec3  uTint;\n"
"uniform float uHue;\n"          // radians
"uniform float uBright;\n"
"void main() {\n"
"    vec2 c = (fragTexCoord - 0.5) / uZoom;\n"
"    c += uWarp * vec2(sin(c.y * 9.0 + uTime * 0.50) + 0.5 * sin(c.x * 17.0 - uTime * 0.70),\n"
"                      sin(c.x * 8.0 - uTime * 0.43) + 0.5 * sin(c.y * 15.0 + uTime * 0.61));\n"
"    vec2 uv = c + 0.5;\n"
"    vec3 col = mix(texture2D(uPrev, uv).rgb, texture2D(texture0, uv).rgb, uMix);\n"
"    vec3 k = vec3(0.57735);\n"
"    float ch = cos(uHue), sh = sin(uHue);\n"
"    col = col * ch + cross(k, col) * sh + k * dot(k, col) * (1.0 - ch);\n"
"    col = mix(vec3(dot(col, vec3(0.299, 0.587, 0.114))), col, uSat);\n"
"    col = clamp((col - 0.5) * uContrast + 0.5, 0.0, 1.0) * uBright * uTint;\n"
"    gl_FragColor = vec4(col, uOp);\n"
"}\n";

// final pass over the whole picture: hue rotation, saturation, contrast, brightness, tint, film grain
static const char *FS_GRADE =
"uniform sampler2D texture0;\n"
"uniform float uHue;\n"          // radians
"uniform float uSat;\n"
"uniform float uBright;\n"
"uniform float uContrast;\n"
"uniform vec3  uTint;\n"
"uniform float uGrain;\n"        // amplitude
"uniform float uGrainSize;\n"    // screen pixels
"uniform float uGrainSeed;\n"    // seed + frame, kept small for float precision
"uniform float uGrainColor;\n"   // 0 = grey, 1 = coloured
"float hash(vec2 p) {\n"         // Dave Hoskins' hash12
"    vec3 p3 = fract(vec3(p.xyx) * 0.1031);\n"
"    p3 += dot(p3, p3.yzx + 33.33);\n"
"    return fract((p3.x + p3.y) * p3.z);\n"
"}\n"
"void main() {\n"
"    vec3 c = texture2D(texture0, fragTexCoord).rgb;\n"
"    vec3 k = vec3(0.57735);\n"
"    float ch = cos(uHue), sh = sin(uHue);\n"
"    c = c * ch + cross(k, c) * sh + k * dot(k, c) * (1.0 - ch);\n"   // rotate around the grey axis
"    c = mix(vec3(dot(c, vec3(0.299, 0.587, 0.114))), c, uSat);\n"
"    c = ((c - 0.5) * uContrast + 0.5) * uBright * uTint;\n"
"    vec2 g = floor(gl_FragCoord.xy / max(uGrainSize, 1.0)) + uGrainSeed * vec2(13.7, 7.3);\n"
"    vec3 n = mix(vec3(hash(g)), vec3(hash(g + 3.1), hash(g + 7.7), hash(g + 11.3)), uGrainColor) - 0.5;\n"
"    c += 2.0 * uGrain * n;\n"
"    gl_FragColor = vec4(clamp(c, 0.0, 1.0), 1.0);\n"
"}\n";

// palette sliders (G): applied on top of the show file
static const struct { const char *name; float lo, hi, def; } GRADE_UI[] = {
    { "hue",        -180, 180, 0 }, { "saturation", 0, 2, 1 }, { "brightness", 0, 2, 1 }, { "contrast", 0.5f, 2, 1 },
    { "warm - cold", -1, 1, 0 },    { "green - magenta", -1, 1, 0 }, { "video", 0, 2, 1 }, { "grain", 0, 3, 1 },
};
#define N_GRADE ((int)(sizeof GRADE_UI/sizeof GRADE_UI[0]))
enum { G_HUE, G_SAT, G_BRIGHT, G_CONTRAST, G_WARM, G_MAGENTA, G_VIDEO, G_GRAIN };

// phone remote (remote.c): per-layer grade on top of the show file. Saved to remote.txt next to show.txt.
// Colour wheels: position (wx, wy) in the unit circle = tint towards that hue, stronger further from the centre.
static const struct { const char *key, *name; float lo, hi, def; } RV[] = {
    { "vz_hue", "hue", -180, 180, 0 }, { "vz_sat", "sat", 0, 2, 1 }, { "vz_bright", "bright", 0, 2, 1 },
    { "vz_con", "contrast", 0.5f, 2, 1 }, { "vz_wx", "", -1, 1, 0 }, { "vz_wy", "", -1, 1, 0 },
    { "vd_op", "opacity", 0, 2.5f, 1 }, { "vd_hue", "hue", -180, 180, 0 }, { "vd_sat", "sat", 0, 2, 1 },
    { "vd_bright", "bright", 0, 2, 1 }, { "vd_con", "contrast", 0.5f, 2, 1 }, { "vd_wx", "", -1, 1, 0 },
    { "vd_wy", "", -1, 1, 0 }, { "grain", "grain", 0, 3, 1 }, { "beat", "beat", 0, 1.5f, BEAT_AMT },
};
#define N_RV ((int)(sizeof RV/sizeof RV[0]))
enum { R_ZHUE, R_ZSAT, R_ZBRIGHT, R_ZCON, R_ZWX, R_ZWY, R_DOP, R_DHUE, R_DSAT, R_DBRIGHT, R_DCON, R_DWX, R_DWY, R_GRAIN, R_BEAT };

static Vector3 WheelTint(float x, float y)
{
    float r = sqrtf(x*x + y*y);
    if (r > 1) { x /= r; y /= r; r = 1; }
    float h = atan2f(y, x)/6.2831853f; h -= floorf(h);
    float c[3];
    for (int i = 0; i < 3; i++) {   // fully saturated hue, same formula as the wheel on the phone
        float k = fmodf(5 - 2*i + h*6, 6);
        c[i] = 1 - fmaxf(0, fminf(fminf(k, 4 - k), 1));
    }
    float l = 0.299f*c[0] + 0.587f*c[1] + 0.114f*c[2];
    return (Vector3){ 1 + 0.75f*r*(c[0] - l), 1 + 0.75f*r*(c[1] - l), 1 + 0.75f*r*(c[2] - l) };
}

static void RemoteLoad(const char *path, float *rv)
{
    FILE *f = fopen(path, "r");
    char key[32]; float v;
    while (f && fscanf(f, "%31s %f", key, &v) == 2)
        for (int i = 0; i < N_RV; i++) if (!strcmp(key, RV[i].key)) rv[i] = fmaxf(RV[i].lo, fminf(RV[i].hi, v));
    if (f) fclose(f);
}

static void RemoteSave(const char *path, const float *rv)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# phone remote values, written by overtone_mandel\n");
    for (int i = 0; i < N_RV; i++) fprintf(f, "%s %g\n", RV[i].key, rv[i]);
    fclose(f);
}

static Shader LoadShaderBody(const char *body);
static Shader LoadMandelShader(void) { return LoadShaderBody(FS_BODY); }
static Shader LoadShaderBody(const char *body)
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
#if defined(PLATFORM_ANDROID)
    const char *iter = "#define ORG_ITER 14\n";
#else
    const char *iter = "#define ORG_ITER 18\n";
#endif
    size_t n = strlen(hdr) + strlen(iter) + strlen(body) + 1;
    char *src = malloc(n);
    strcpy(src, hdr); strcat(src, iter); strcat(src, body);
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

// Base-tone slider: the leftmost ZERO_U of the track is "0 Hz" (any voice counts as above the base),
// the rest is a log scale F0_LO..F0_HI.
#define ZERO_U 0.06f
static float SliderU(float f) { return f <= 0 ? 0 : ZERO_U + (1 - ZERO_U)*Clamp01(logf(f/F0_LO)/logf(F0_HI/F0_LO)); }
static float SliderF(float u) { return u < ZERO_U*0.5f ? 0 : F0_LO*powf(F0_HI/F0_LO, Clamp01((u - ZERO_U)/(1 - ZERO_U))); }

// Voice drive in -1..1: above the base -> positive, below -> negative, dead zone around it.
// With the base at 0 Hz every voice is "above" and loudness sets the strength.
static float VoiceDrive(float f0, float base, float level)
{
    if (base <= 0) return Clamp01(level*8);
    float semi = 12*log2f(f0/base);
    float a = Clamp01((fabsf(semi) - DEAD_SEMI)/(FULL_SEMI - DEAD_SEMI));
    return semi > 0 ? a : -a;
}

static float Median5(float *h, float v)
{
    for (int i = 0; i < 4; i++) h[i] = h[i + 1];
    h[4] = v;
    float s[5]; memcpy(s, h, sizeof s);
    for (int i = 1; i < 5; i++) for (int j = i; j > 0 && s[j] < s[j-1]; j--) { float t = s[j]; s[j] = s[j-1]; s[j-1] = t; }
    return s[2];
}

int main(int argc, char **argv)
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
    int locPulse = GetShaderLocation(sh, "uPulse"), locGlow = GetShaderLocation(sh, "uGlow");
    struct { Shader s; int res, time, rot, slice, c, zoom, hue, pulse, glow; } live[3];
    const char *liveSrc[3] = { FS_ORG, FS_NEWTON, FS_APOLLO };
    for (int i = 0; i < 3; i++) {
        Shader q = live[i].s = LoadShaderBody(liveSrc[i]);
        live[i].res = GetShaderLocation(q, "uRes");     live[i].time = GetShaderLocation(q, "uTime");
        live[i].rot = GetShaderLocation(q, "uRot");     live[i].slice = GetShaderLocation(q, "uSlice");
        live[i].c = GetShaderLocation(q, "uC");         live[i].zoom = GetShaderLocation(q, "uZoom");
        live[i].hue = GetShaderLocation(q, "uHue");     live[i].pulse = GetShaderLocation(q, "uPulse");
        live[i].glow = GetShaderLocation(q, "uGlow");
    }
    float flowT = 0, rotA = 0, rotB = 0, rotV = 0, kick = 0, envFast = 0, envSlow = 0, interiorT = 0;
    // beat: tempo tracker fed from the same audio ring; beatAmt = strength of the beat pulse (slider, [ ])
    float beatSr = 0, beatAmt = BEAT_AMT, beatPrevPhase = 0, pulseFx = 0;
    unsigned beatPos = 0;
    if (getenv("BEAT")) beatAmt = (float)atof(getenv("BEAT"));
    int beatDrag = 0;
    // physarum overlay (CPU), toggled with P / button
    int physOn = getenv("PHYSARUM") != NULL, physReady = 0;
    Texture2D physTex = { 0 };
    // chaos for physarum: the note heard most often (strongest spectral peak, weighted by level) over ~1 s
    struct { int note; float w; double t; } noteHist[96] = { 0 };
    // video layer + colour grade + grain, driven by the show file. Media folder (show.txt + videos):
    // first argument, else MEDIA=<dir>, else ./media; SHOW=<file or dir> overrides the show file,
    // VIDEO=<file> plays just that clip. V toggles the video, , . switch clips, G shows the palette sliders.
    Shader filmSh = LoadShaderBody(FS_FILM);
    int locFilmT = GetShaderLocation(filmSh, "uTime"), locFilmOp = GetShaderLocation(filmSh, "uOp");
    int locFilmPrev = GetShaderLocation(filmSh, "uPrev"), locFilmMix = GetShaderLocation(filmSh, "uMix");
    int locFilmWarp = GetShaderLocation(filmSh, "uWarp"), locFilmZoom = GetShaderLocation(filmSh, "uZoom");
    int locFilmSat = GetShaderLocation(filmSh, "uSat"), locFilmCon = GetShaderLocation(filmSh, "uContrast");
    int locFilmTint = GetShaderLocation(filmSh, "uTint");
    int locFilmHue = GetShaderLocation(filmSh, "uHue"), locFilmBright = GetShaderLocation(filmSh, "uBright");
    // the visualisation (scene + physarum) gets its own grade: the same shader as the final pass, without grain
    Shader vizSh = LoadShaderBody(FS_GRADE);
    int locZHue = GetShaderLocation(vizSh, "uHue"), locZSat = GetShaderLocation(vizSh, "uSat");
    int locZBright = GetShaderLocation(vizSh, "uBright"), locZCon = GetShaderLocation(vizSh, "uContrast");
    int locZTint = GetShaderLocation(vizSh, "uTint"), locZGrain = GetShaderLocation(vizSh, "uGrain");
    Shader gradeSh = LoadShaderBody(FS_GRADE);
    int locGHue = GetShaderLocation(gradeSh, "uHue"), locGSat = GetShaderLocation(gradeSh, "uSat");
    int locGBright = GetShaderLocation(gradeSh, "uBright"), locGCon = GetShaderLocation(gradeSh, "uContrast");
    int locGTint = GetShaderLocation(gradeSh, "uTint"), locGGrain = GetShaderLocation(gradeSh, "uGrain");
    int locGSize = GetShaderLocation(gradeSh, "uGrainSize"), locGSeed = GetShaderLocation(gradeSh, "uGrainSeed");
    int locGColor = GetShaderLocation(gradeSh, "uGrainColor");
    Image vidImg = GenImageColor(VID_W, VID_H, BLACK);
    Texture2D vidTex[2] = { LoadTextureFromImage(vidImg), LoadTextureFromImage(vidImg) };   // current, previous clip
    UnloadImage(vidImg);
    for (int i = 0; i < 2; i++) { SetTextureFilter(vidTex[i], TEXTURE_FILTER_BILINEAR); SetTextureWrap(vidTex[i], TEXTURE_WRAP_CLAMP); }
    const char *mediaDir = argc > 1 ? argv[1] : getenv("MEDIA") ? getenv("MEDIA") : "media";
    ShowInit(getenv("SHOW") ? getenv("SHOW") : mediaDir, getenv("VIDEO"), VID_W, VID_H);
    float showP[N_SP] = { 0 }, showV[N_SV] = { 0 }, dbS = -90;
    float gradeV[N_GRADE];
    for (int i = 0; i < N_GRADE; i++) gradeV[i] = GRADE_UI[i].def;
    int gradeUi = 0, gradeDrag = -1;
    RenderTexture2D rtOut = { 0 }, rtViz = { 0 };   // the whole picture before the grade pass; the visualisation alone
    int outW = 0, outH = 0;
    // phone remote: values (rv) on top of the show file, saved next to it
    float rv[N_RV];
    for (int i = 0; i < N_RV; i++) rv[i] = RV[i].def;
    char rvPath[600];
    snprintf(rvPath, sizeof rvPath, "%s/remote.txt", ShowDir());
    RemoteLoad(rvPath, rv);
    double rvSaveAt = -1;
    RemoteStart(getenv("REMOTE_PORT") ? atoi(getenv("REMOTE_PORT")) : REMOTE_PORT);
    int stateTick = 0;
    int noteW = 0, domNote = 0; float chaosU = 0;

    Image refImg = { refBuf, REF_W, REF_H, 1, PIXELFORMAT_UNCOMPRESSED_R32G32B32A32 };
    Texture2D refTex = LoadTextureFromImage(refImg);
    SetTextureFilter(refTex, TEXTURE_FILTER_POINT);

    RenderTexture2D rt = { 0 }, rtPrev = { 0 };
    int rtW = 0, rtH = 0;

    double cx = START_X, cy = START_Y, scale = START_SCALE;
    mandelOn = getenv("MANDEL") != NULL;
    int scene = getenv("SCENE") ? atoi(getenv("SCENE")) % N_SCENES
              : getenv("ORGANISM") ? 2 : getenv("COLOR_MODE") ? 1 : 0;
    if (!SceneOn(scene)) scene = 2;
    int prevScene = -1, autoOn = getenv("AUTO") != NULL, hideUi = getenv("HIDE_UI") != NULL;
    if (hideUi) HideCursor();
    float fadeT = FADE_SEC, sceneT = 0, sceneSec = getenv("SCENE_SEC") ? (float)atof(getenv("SCENE_SEC")) : SCENE_SEC;
    int colorMode = SCENES[scene].mode == MODE_DIVE;   // dive into DIVES[], overtone shifts colour
    int organism = SCENES[scene].mode >= MODE_ORG;    // a "living" scene: slice/mutation controls instead of zoom/pan
    int dive = getenv("DIVE") ? atoi(getenv("DIVE")) % N_DIVES : 0;
    int ascending = 0; float limitT = 0, pulse = 0;
    float slice = getenv("ORG_SLICE") ? (float)atof(getenv("ORG_SLICE")) : ORG_SLICE0, sliceV = 0, mutX = 0, mutY = 0;
    float orgCx[3] = { 0 }; int orgCOverride = 0;
    float orgZoomEnv = getenv("ORG_ZOOM") ? (float)atof(getenv("ORG_ZOOM")) : 0;
    if (getenv("ORG_C")) orgCOverride = sscanf(getenv("ORG_C"), "%f,%f,%f", &orgCx[0], &orgCx[1], &orgCx[2]) == 3;   // tuning hook
    if (colorMode) { cx = DIVES[dive].x; cy = DIVES[dive].y; }
    if (getenv("TEST_SCALE")) scale = atof(getenv("TEST_SCALE"));   // debug hook
    float hist[5] = { 0 }, f0s = 0, otS = 0, promS = 0, level = 0;
    float zoomRate = 0, vx = 0, vy = 0, otAngle = 0;
    float f0Base = F0_BASE;
    if (getenv("F0_BASE")) f0Base = (float)atof(getenv("F0_BASE"));
    int sliderDrag = 0;
    static float specS[VOICE_SPEC_BINS], harmS[VOICE_HMAX];   // smoothed display spectrum / harmonic levels, dB
    for (int b = 0; b < VOICE_SPEC_BINS; b++) specS[b] = -200;
    int nHarmS = 0, otN = 0;
    double lockSeen = 0;
    float otLock = 0;                 // captured overtone frequency (Hz), hysteresis against flicker
    float dirX = 0, dirY = 1, rawAngle = 0;
    float harmVis = 0, specRef = -200;
    int voiced = 0, analysisTick = 0;
    double permTimer = 0; int asked = 0;
    float window[VOICE_N];
    beatAmt = getenv("BEAT") ? beatAmt : rv[R_BEAT];
    double startTime = GetTime(), fileStart = startTime;

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        if (dt > 0.1f) dt = 0.1f;

        // ---- audio source
        if (fileData) {
            double want = (GetTime() - fileStart)*sampleRate;
            unsigned n = 0;
            while (filePos < want && filePos < fileWave.frameCount) { filePos++; n++; }
            unsigned off = (unsigned)filePos - n;
            if (n) PushSamples(fileData + off, n);
            if (filePos >= fileWave.frameCount) {
                if (getenv("VOICE_LOOP")) { filePos = 0; fileStart = GetTime(); }   // debug: replay the file forever
                else { float z[1024] = {0}; PushSamples(z, 1024); }
            }
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
                if (v.rms > 0.003f && v.peakFreq > 0) {
                    noteHist[noteW % 96].note = (int)lroundf(12*log2f(v.peakFreq/440.0f) + 69);
                    noteHist[noteW % 96].w = v.rms;
                    noteHist[noteW % 96].t = GetTime();
                    noteW++;
                }
                voiced = v.voiced && v.rms > 0.003f;
                if (voiced) {
                    float f = Median5(hist, v.f0);
                    f0s = f0s > 0 ? f0s + (f - f0s)*0.35f : f;
                    float p = Clamp01((v.otProm - PROM_LO)/(PROM_HI - PROM_LO));
                    promS += (p - promS)*0.3f;
                    if (p > 0 && v.otFreq > 0) {
                        // Captured in Hz, not by harmonic number: f0 octave errors don't move it.
                        // It moves to another overtone only when it hasn't been seen for OT_SWITCH s,
                        // so flicker between harmonics and single stray frames are ignored, glides follow.
                        float tol = 0.5f*f0s;
                        double now = GetTime();
                        if (otLock == 0 || fabsf(v.otFreq - otLock) < tol || now - lockSeen >= OT_SWITCH) {
                            otLock = v.otFreq; lockSeen = now;
                        }
                        otS = otS > 0 ? otS*powf(otLock/otS, 0.3f) : otLock;
                        otN = (int)lroundf(otLock/f0s);
                        rawAngle = 6.2831853f*logf(v.otFreq/OT_LO)/logf(OT_HI/OT_LO) + 1.5707963f;
                    }
                    for (int n = 0; n < v.nHarm; n++)
                        harmS[n] = n < nHarmS ? harmS[n] + (v.harmDb[n] - harmS[n])*0.5f : v.harmDb[n];
                    nHarmS = v.nHarm;
                } else {
                    promS *= 0.7f;
                }
                for (int b = 0; b < VOICE_SPEC_BINS; b++)
                    specS[b] = v.hasSpec ? specS[b] + (v.spec[b] - specS[b])*0.5f : specS[b] - 3;
                harmVis = voiced ? harmVis + (1 - harmVis)*0.5f : harmVis*0.75f;
            }
        }

        // ---- base-tone slider (bottom of the screen, log scale F0_LO..F0_HI)
        int sw = GetScreenWidth(), shh = GetScreenHeight();
        int fsz = shh/30 > 14 ? shh/30 : 14;
        Rectangle track = { (float)fsz*3, (float)(shh - fsz*2.2f), (float)(sw - fsz*6), (float)fsz*0.5f };
        Rectangle hit = { track.x - fsz, track.y - fsz*1.5f, track.width + fsz*2, track.height + fsz*3 };
        Rectangle modeBtn = { fsz*0.5f, fsz*7.9f, fsz*16.0f, fsz*1.8f };   // under the HUD box
        Rectangle physBtn = { fsz*0.5f, fsz*10.1f, fsz*16.0f, fsz*1.8f };
        Rectangle beatRow = { fsz*0.5f, fsz*12.3f, fsz*16.0f, fsz*1.8f };
        Rectangle beatTrack = { beatRow.x + fsz*9.8f, beatRow.y + fsz*0.65f, fsz*5.6f, fsz*0.5f };
        Rectangle autoBtn = { fsz*0.5f, fsz*14.5f, fsz*16.0f, fsz*1.8f };
        Rectangle gradeBox = { sw*0.5f - fsz*10, fsz*0.5f, fsz*20.0f, fsz*(2.0f + 1.4f*N_GRADE) };
        // hidden interface: its buttons and sliders must not catch clicks (mouse "parked" far away for hit tests)
        Vector2 mp = hideUi ? (Vector2){ -1e6f, -1e6f } : GetMousePosition();
        int onUi = (gradeUi && CheckCollisionPointRec(mp, gradeBox)) || CheckCollisionPointRec(mp, hit) || CheckCollisionPointRec(mp, modeBtn) ||
                   CheckCollisionPointRec(mp, physBtn) || CheckCollisionPointRec(mp, beatRow) || CheckCollisionPointRec(mp, autoBtn);
        if (IsKeyPressed(KEY_P) || (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mp, physBtn))) physOn = !physOn;
        if (IsKeyPressed(KEY_V)) ShowSetVideo(!ShowVideoOn(), GetTime());
        if (IsKeyPressed(KEY_PERIOD)) ShowSkip(1, GetTime());
        if (IsKeyPressed(KEY_COMMA))  ShowSkip(-1, GetTime());
        if (IsKeyPressed(KEY_G)) gradeUi = !gradeUi;
        {   // palette sliders: drag, right click = reset; values printed on release to copy into the show file
            int showing = gradeUi && !hideUi;
            for (int i = 0; showing && i < N_GRADE; i++) {
                Rectangle row = { gradeBox.x, gradeBox.y + fsz*(1.6f + 1.4f*i), gradeBox.width, fsz*1.4f };
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mp, row)) gradeDrag = i;
                if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && CheckCollisionPointRec(mp, row)) gradeV[i] = GRADE_UI[i].def;
            }
            if (gradeDrag >= 0 && !IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                TraceLog(LOG_INFO, "GRADE: hue %+.0f  sat x%.2f  bright x%.2f  contrast x%.2f  warm %+.2f  magenta %+.2f  video x%.2f  grain x%.2f",
                         gradeV[0], gradeV[1], gradeV[2], gradeV[3], gradeV[4], gradeV[5], gradeV[6], gradeV[7]);
                gradeDrag = -1;
            }
            if (gradeDrag >= 0) {
                float u = Clamp01((mp.x - (gradeBox.x + fsz*8)) / (gradeBox.width - fsz*11));
                gradeV[gradeDrag] = GRADE_UI[gradeDrag].lo + u*(GRADE_UI[gradeDrag].hi - GRADE_UI[gradeDrag].lo);
            }
        }
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mp, beatRow)) beatDrag = 1;
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) beatDrag = 0;
        if (beatDrag) beatAmt = 1.5f*Clamp01((mp.x - beatTrack.x)/beatTrack.width);
        if (IsKeyPressed(KEY_RIGHT_BRACKET)) beatAmt = fminf(1.5f, beatAmt + 0.1f);
        if (IsKeyPressed(KEY_LEFT_BRACKET))  beatAmt = fmaxf(0.0f, beatAmt - 0.1f);
        {   // scenes: M = next one, A = auto show, H / F11 = hide the interface and cursor (for an audience / streaming)
            int next = -1;
            if (IsKeyPressed(KEY_M) || (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mp, modeBtn))) {
                next = (scene + 1) % N_SCENES;
                while (!SceneOn(next)) next = (next + 1) % N_SCENES;
            }
            {   // phone remote commands
                RemoteCmd cmd[64];
                int n = RemotePoll(cmd, 64);
                for (int i = 0; i < n; i++) {
                    const char *k = cmd[i].key; float v = cmd[i].val;
                    if (!strcmp(k, "scene")) { int s = (int)v; if (s >= 0 && s < N_SCENES && SceneOn(s) && s != scene) next = s; }
                    else if (!strcmp(k, "phys")) physOn = v != 0;
                    else if (!strcmp(k, "video")) { if ((v != 0) != ShowVideoOn()) ShowSetVideo(v != 0, GetTime()); }
                    else if (!strcmp(k, "clip")) ShowSkip(v < 0 ? -1 : 1, GetTime());
                    else if (!strcmp(k, "auto")) { autoOn = v != 0; sceneT = 0; }
                    else if (!strcmp(k, "hide")) { hideUi = v != 0; if (hideUi) HideCursor(); else ShowCursor(); }
                    else if (!strcmp(k, "reset_vz") || !strcmp(k, "reset_vd")) {
                        for (int j = 0; j < N_RV; j++) if (!strncmp(RV[j].key, k + 6, 2)) rv[j] = RV[j].def;
                        rvSaveAt = GetTime() + 2;
                    } else
                        for (int j = 0; j < N_RV; j++) if (!strcmp(k, RV[j].key)) {
                            rv[j] = fmaxf(RV[j].lo, fminf(RV[j].hi, v));
                            if (j == R_BEAT) beatAmt = rv[j];
                            rvSaveAt = GetTime() + 2;
                        }
                }
                if (rvSaveAt > 0 && GetTime() > rvSaveAt) { RemoteSave(rvPath, rv); rvSaveAt = -1; }
            }
            if (IsKeyPressed(KEY_A) || (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mp, autoBtn))) {
                autoOn = !autoOn; sceneT = 0;
            }
            if (IsKeyPressed(KEY_H) || IsKeyPressed(KEY_F11)) {
                hideUi = !hideUi;
                if (hideUi) HideCursor(); else ShowCursor();
            }
            sceneT += dt; fadeT += dt;
            if (autoOn && sceneT >= sceneSec && fadeT >= FADE_SEC) {
                int k = 0;
                for (int i = 0; i < N_SHOW; i++) if (SHOW[i] == scene) k = i;
                do k = (k + 1) % N_SHOW; while (!SceneOn(SHOW[k]));
                next = SHOW[k];
            }
            if (next >= 0) {
                int bothMandel = SCENES[next].mode <= MODE_DIVE && SCENES[scene].mode <= MODE_DIVE;
                prevScene = bothMandel ? -1 : scene;          // two Mandelbrot views can't be drawn at once
                fadeT = bothMandel ? FADE_SEC : 0;
                scene = next; sceneT = 0;
                if (SCENES[next].mode == MODE_DIVE) { cx = DIVES[dive].x; cy = DIVES[dive].y; scale = START_SCALE; vx = vy = 0; ascending = 0; }
            }
            colorMode = SCENES[scene].mode == MODE_DIVE;
            organism = SCENES[scene].mode >= MODE_ORG;
        }
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mp, hit)) sliderDrag = 1;
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) sliderDrag = 0;
        if (sliderDrag) {
            f0Base = SliderF(Clamp01((GetMousePosition().x - track.x)/track.width));
        }
        if (IsKeyPressed(KEY_UP))   f0Base = f0Base <= 0 ? F0_LO : f0Base*1.0594631f;
        if (IsKeyPressed(KEY_DOWN)) f0Base = f0Base/1.0594631f < F0_LO*0.999f ? 0 : f0Base/1.0594631f;
        if (f0Base > 0 && f0Base < F0_LO) f0Base = F0_LO;
        if (f0Base > F0_HI) f0Base = F0_HI;

        // ---- controls from voice: above the base tone zoom in, below zoom out
        float targetZoom = 0;
        if (voiced && f0s > 0) {
            float d = VoiceDrive(f0s, f0Base, level);
            targetZoom = d > 0 ? ZOOM_MAX*d : ZOOM_OUT_MAX*d;
            if (ascending) targetZoom = -(ASCENT_MIN + (ASCENT_MAX - ASCENT_MIN)*fabsf(d));   // any voice only rises
        }
        if (!sliderDrag && !beatDrag && (IsKeyDown(KEY_SPACE) || (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && !onUi && GetGestureHoldDuration() > 0.35f)))
            targetZoom = -1.0f;
        zoomRate += (targetZoom - zoomRate)*(1 - expf(-dt*6));

        float tvx = 0, tvy = 0;
        if (otS > 0) {
            float u = logf(otS/OT_LO)/logf(OT_HI/OT_LO);
            float ta = 6.2831853f*u + 1.5707963f;       // 500 Hz -> вверх, далее против часовой
            // turn the direction as a vector: no wrap-around jumps, slower when the overtone is weak
            float k = 1 - expf(-dt*DIR_RATE*(0.25f + 0.75f*promS));
            dirX += (cosf(ta) - dirX)*k;
            dirY += (sinf(ta) - dirY)*k;
            float len = sqrtf(dirX*dirX + dirY*dirY);
            if (len > 1e-4f) otAngle = atan2f(dirY, dirX);
            if (!colorMode) {
                tvx = dirX*promS*PAN_MAX;
                tvy = dirY*promS*PAN_MAX;
            }
        }
        vx += (tvx - vx)*(1 - expf(-dt*VEL_RATE));
        vy += (tvy - vy)*(1 - expf(-dt*VEL_RATE));

        if (organism) {
            // pitch above/below the base moves the slice forward/back through the 3rd dimension
            float tsv = 0;
            if (voiced && f0s > 0) {
                tsv = VoiceDrive(f0s, f0Base, level)*SLICE_MAX;
            }
            sliceV += (tsv - sliceV)*(1 - expf(-dt*2.0f));
            slice += (sliceV + 0.02f)*dt;             // plus a slow drift: never fully still
            // the overtone walks C through parameter space; the organism keeps its mutation
            mutX += dirX*promS*MUT_SPEED*dt;
            mutY += dirY*promS*MUT_SPEED*dt;
            // slice and mutation accumulate freely, the shader gets them through sin(): a pendulum that
            // never runs off into the empty far region (long music used to push it there)
            vx = vy = 0; zoomRate = 0;
        }

        if (colorMode) {
            vx = vy = 0;
            if (!ascending) { cx = DIVES[dive].x; cy = DIVES[dive].y; }
            else {   // glide to the next point at a speed proportional to the view size
                double ddx = DIVES[dive].x - cx, ddy = DIVES[dive].y - cy, d = sqrt(ddx*ddx + ddy*ddy);
                double step = ASCENT_PAN*scale*dt;
                if (step >= d) { cx = DIVES[dive].x; cy = DIVES[dive].y; }
                else { cx += ddx/d*step; cy += ddy/d*step; }
            }
        }
        cx += vx*scale*dt;
        cy += vy*scale*dt;
        double minScale = 2e-13*fmax(1.0, sqrt(cx*cx + cy*cy));
        scale *= exp(-zoomRate*dt);
        int atLimit = scale <= minScale;
        if (atLimit) scale = minScale;
        if (scale > 4) scale = 4;

        if (organism) atLimit = 0;
        // dive cycle: hold at the precision limit, then rise (and in colour mode move to the next point)
        limitT = atLimit && !ascending && !organism ? limitT + dt : 0;
        if (limitT >= LIMIT_HOLD) {
            ascending = 1; limitT = 0;
            if (colorMode) dive = (dive + 1) % N_DIVES;
        }
        if (ascending && scale >= ASCENT_TOP &&
            (!colorMode || (cx == DIVES[dive].x && cy == DIVES[dive].y))) ascending = 0;

        if (IsKeyPressed(KEY_R) || (IsGestureDetected(GESTURE_DOUBLETAP) && !onUi)) {
            cx = colorMode ? DIVES[dive].x : START_X; cy = colorMode ? DIVES[dive].y : START_Y; scale = START_SCALE;
            ascending = 0;
            if (organism) { slice = ORG_SLICE0; sliceV = 0; mutX = mutY = 0; }
        }

        // ---- render
        int w = (int)(sw*RENDER_SCALE), h = (int)(shh*RENDER_SCALE);
        if (w != rtW || h != rtH) {
            if (rtW) UnloadRenderTexture(rt);
            if (rtW) UnloadRenderTexture(rtPrev);
            rt = LoadRenderTexture(w, h); rtW = w; rtH = h;
            rtPrev = LoadRenderTexture(w, h);
            SetTextureFilter(rt.texture, TEXTURE_FILTER_BILINEAR);
            SetTextureFilter(rtPrev.texture, TEXTURE_FILTER_BILINEAR);
        }

        float depth = (float)log2(START_SCALE/scale);
        int maxIter = 250 + (int)(depth*60);
        if (maxIter > ITER_CAP) maxIter = ITER_CAP;
        int fading = prevScene >= 0 && fadeT < FADE_SEC;
        int needMandel = !organism || (fading && SCENES[prevScene].mode <= MODE_DIVE);
        int refLen = needMandel ? ComputeRef(cx, cy, maxIter) : 0;
        if (needMandel) UpdateTexture(refTex, refBuf);

        float res[2] = { (float)w, (float)h }, fs = (float)scale, fi = (float)maxIter, fl = (float)refLen;
        // colour mode: the (smoothed) overtone direction is a position on the palette circle
        float hue0 = (float)(depth*0.02 + GetTime()*0.01);
        {   // loudness -> breathing (softer attack: less flicker on music), onsets -> kick
            float lt = Clamp01(level*8);
            pulse += (lt - pulse)*(1 - expf(-dt*(lt > pulse ? 6.0f : 1.8f)));
            envFast += (lt - envFast)*(1 - expf(-dt*25.0f));
            envSlow += (lt - envSlow)*(1 - expf(-dt*1.5f));
            if (envFast - envSlow*1.25f > 0.08f && kick < 0.3f) kick = 1;
            kick *= expf(-dt*3.5f);
        }
        // ---- beat: feed new audio to the tempo tracker, turn its phase into a pulse on every beat
        if (beatSr != sampleRate) { BeatInit(sampleRate); beatSr = sampleRate; beatPos = __atomic_load_n(&ringW, __ATOMIC_ACQUIRE); }
        {
            unsigned wr = __atomic_load_n(&ringW, __ATOMIC_ACQUIRE);
            if (wr - beatPos > RING/2) beatPos = wr - RING/2;
            while (beatPos != wr) {
                unsigned i = beatPos & (RING - 1), n = wr - beatPos;
                if (n > RING - i) n = RING - i;
                BeatPush(ring + i, (int)n);
                beatPos += n;
            }
        }
        BeatInfo bt = BeatGet();
        float beatOn = bt.bpm > 0 ? Clamp01((bt.conf - 0.25f)/0.35f) : 0;     // fade in only when the tempo is clear
        float beatEnv = beatOn*expf(-bt.phase*7.0f);
        int beatHit = beatOn > 0.3f && bt.phase < beatPrevPhase;              // phase wrapped: a new beat
        beatPrevPhase = bt.phase;
        pulseFx = Clamp01(pulse + beatAmt*beatEnv*0.9f);
        fs = (float)(scale*(1 - 0.05f*beatAmt*beatEnv));                      // small zoom punch on the beat
        // drift mode: centre fell into the black interior (only pulsing left) -> rise out of it
        interiorT = !organism && !colorMode && !ascending && refLen >= maxIter && scale < 1e-2 ? interiorT + dt : 0;
        if (interiorT > 2.0f) { ascending = 1; interiorT = 0; }
        // motion of the living scenes: flow time and 3D rotation speed up with loudness, onsets give a jolt
        flowT += dt*(1 + 2.5f*pulse);
        rotV += (0.03f + 0.20f*pulse - rotV)*(1 - expf(-dt*1.5f));
        rotA = fmodf(rotA + (rotV + 0.9f*kick)*dt, 62.831853f);
        rotB = fmodf(rotB + (0.6f*rotV + 0.5f*kick*dirY)*dt, 62.831853f);
        float tm = fmodf(flowT, 10000.0f);
        float effSlice = SLICE_C + SLICE_A*sinf(slice/SLICE_A);
        float rot2[2] = { rotA, rotB };
        float liveHue = (float)(tm*0.004) + otAngle/6.2831853f*0.35f;

        // draw the current scene into rt and, during a cross-fade, the previous one into rtPrev
        for (int L = 0; L < 1 + fading; L++) {
            int sc = L ? prevScene : scene, lm = SCENES[sc].mode;
            BeginTextureMode(L ? rtPrev : rt);
            if (lm >= MODE_ORG) {
                const float *c0 = lm == MODE_ORG ? (orgCOverride ? orgCx : ORG_PRESETS[SCENES[sc].preset].c) : (const float[3]){ 0, 0, 0 };
                float zb = lm == MODE_ORG ? (orgZoomEnv > 0 ? orgZoomEnv : ORG_PRESETS[SCENES[sc].preset].zoom) : 1.0f;
                float C[3] = { c0[0] + MUT_R*sinf(mutX/MUT_R) + 0.02f*sinf(tm*0.11f),
                               c0[1] + MUT_R*sinf(mutY/MUT_R) + 0.02f*sinf(tm*0.07f + 1.0f),
                               c0[2] + 0.015f*sinf(tm*0.05f + 2.0f) };
                float zoom = zb*(1 + 0.05f*sinf(tm*0.09f))*(1 - 0.10f*kick)*(1 - 0.08f*beatAmt*beatEnv);
                int li = lm - MODE_ORG;
                Shader q = live[li].s;
                BeginShaderMode(q);
                    SetShaderValue(q, live[li].res, res, SHADER_UNIFORM_VEC2);
                    SetShaderValue(q, live[li].time, &tm, SHADER_UNIFORM_FLOAT);
                    SetShaderValue(q, live[li].slice, &effSlice, SHADER_UNIFORM_FLOAT);
                    SetShaderValue(q, live[li].rot, rot2, SHADER_UNIFORM_VEC2);
                    SetShaderValue(q, live[li].c, C, SHADER_UNIFORM_VEC3);
                    SetShaderValue(q, live[li].zoom, &zoom, SHADER_UNIFORM_FLOAT);
                    SetShaderValue(q, live[li].hue, &liveHue, SHADER_UNIFORM_FLOAT);
                    SetShaderValue(q, live[li].pulse, &pulseFx, SHADER_UNIFORM_FLOAT);
                    SetShaderValue(q, live[li].glow, &promS, SHADER_UNIFORM_FLOAT);
                    DrawRectangle(0, 0, w, h, WHITE);
                EndShaderMode();
            } else {
                // dive + colour: the (smoothed) overtone direction is a position on the palette circle
                float hue = hue0 + (lm == MODE_DIVE ? otAngle/6.2831853f : 0);
                BeginShaderMode(sh);
                    SetShaderValue(sh, locRes, res, SHADER_UNIFORM_VEC2);
                    SetShaderValue(sh, locScale, &fs, SHADER_UNIFORM_FLOAT);
                    SetShaderValue(sh, locIter, &fi, SHADER_UNIFORM_FLOAT);
                    SetShaderValue(sh, locLen, &fl, SHADER_UNIFORM_FLOAT);
                    SetShaderValue(sh, locHue, &hue, SHADER_UNIFORM_FLOAT);
                    SetShaderValue(sh, locPulse, &pulseFx, SHADER_UNIFORM_FLOAT);
                    SetShaderValue(sh, locGlow, &promS, SHADER_UNIFORM_FLOAT);
                    SetShaderValueTexture(sh, locRef, refTex);
                    DrawRectangle(0, 0, w, h, WHITE);
                EndShaderMode();
            }
            EndTextureMode();
        }

        if (physOn) {
            if (!physReady) {
                PhysInit(PHYS_W, PHYS_H, PHYS_AGENTS, 12345);
                Image im = GenImageColor(PHYS_W, PHYS_H, BLACK);
                ImageFormat(&im, PIXELFORMAT_UNCOMPRESSED_GRAYSCALE);
                physTex = LoadTextureFromImage(im);
                UnloadImage(im);
                SetTextureFilter(physTex, TEXTURE_FILTER_BILINEAR);
                physReady = 1;
            }
            // loudness -> speed and deposit, overtone -> sensor angle (shape of the network), beats -> jolt
            {   // most frequent note of the last second -> chaos character (low: heavy and calm, high: twitchy)
                float acc[128] = { 0 }; double now = GetTime(); int best = 0;
                for (int i = 0; i < 96; i++)
                    if (noteHist[i].w > 0 && now - noteHist[i].t < 1.0 && noteHist[i].note > 0 && noteHist[i].note < 128)
                        acc[noteHist[i].note] += noteHist[i].w;
                for (int n = 1; n < 128; n++) if (acc[n] > acc[best]) best = n;
                if (acc[best] > 0) domNote = best;
                float u = domNote ? Clamp01((domNote - CHAOS_NOTE_LO)/(float)(CHAOS_NOTE_HI - CHAOS_NOTE_LO)) : 0;
                if (acc[best] <= 0) u = 0;                                   // silence: calm down
                chaosU += (u - chaosU)*(1 - expf(-dt*1.5f));
            }
            // sensor angle 25..65 deg: narrow -> long veins and vessels, wide -> honeycomb of cells
            float sa = (25.0f + 40.0f*(0.5f + 0.5f*sinf(otAngle)))*0.017453f;
            float cz = CHAOS_AMT*chaosU;
            PhysParams pp = { sa, 15.0f - 10.0f*chaosU, 30.0f*0.017453f, 0.6f + 1.6f*pulseFx, 1.0f + 2.0f*pulseFx, 0.90f,
                              (beatHit ? 0.05f*beatAmt : 0) + 0.01f*kick,
                              0.05f + 1.6f*powf(cz, 1.3f)*(0.5f + 0.5f*pulseFx),   // wander
                              0.02f + 0.33f*cz,                                     // wild agents
                              0.05f + 0.5f*cz,                                      // swirl strength
                              1.2f + 5.0f*chaosU,                                   // vortex size: low note = big
                              (float)fmod(GetTime(), 10000.0) };
            PhysStep(pp);
            UpdateTexture(physTex, PhysTrail8());
        }

        // ---- show file: variables -> parameters, video playlist
        {
            float db = 20*log10f(level + 1e-5f);
            dbS += (db - dbS)*(1 - expf(-dt*(db > dbS ? 20.0f : 4.0f)));   // fast attack, slower release
            double now = GetTime();
            ShowPoll(now);
            showV[SV_T] = (float)fmod(now, 10000.0); showV[SV_LEVEL] = Clamp01(level*8); showV[SV_DB] = dbS;
            showV[SV_PULSE] = pulseFx; showV[SV_BEAT] = bt.phase; showV[SV_F0] = voiced ? f0s : 0;
            showV[SV_OT] = otS; showV[SV_PROM] = promS; showV[SV_CHAOS] = chaosU; showV[SV_KICK] = kick;
            showV[SV_SCENE] = (float)scene; showV[SV_CLIP] = (float)ShowClipIndex(); showV[SV_CLIPT] = ShowClipTime(now);
            ShowEval(showV, showP, now);
        }
        ShowVideo sv = ShowVideoStep(GetTime(), showP);
        if (sv.swapped) { Texture2D t = vidTex[0]; vidTex[0] = vidTex[1]; vidTex[1] = t; }
        if (sv.cur)  UpdateTexture(vidTex[0], sv.cur);
        if (sv.prev) UpdateTexture(vidTex[1], sv.prev);

        if (sw != outW || shh != outH) {
            if (outW) { UnloadRenderTexture(rtOut); UnloadRenderTexture(rtViz); }
            rtOut = LoadRenderTexture(sw, shh); outW = sw; outH = shh;
            rtViz = LoadRenderTexture(sw, shh);
        }
        rv[R_BEAT] = beatAmt;
        BeginTextureMode(rtViz);
            ClearBackground(BLACK);
            if (fading) {   // previous scene underneath, the new one fades in (smoothstep)
                float f = Clamp01(fadeT/FADE_SEC); f = f*f*(3 - 2*f);
                DrawTexturePro(rtPrev.texture, (Rectangle){ 0, 0, (float)w, (float)-h },
                               (Rectangle){ 0, 0, (float)sw, (float)shh }, (Vector2){ 0, 0 }, 0, WHITE);
                DrawTexturePro(rt.texture, (Rectangle){ 0, 0, (float)w, (float)-h },
                               (Rectangle){ 0, 0, (float)sw, (float)shh }, (Vector2){ 0, 0 }, 0, Fade(WHITE, f));
            } else
                DrawTexturePro(rt.texture, (Rectangle){ 0, 0, (float)w, (float)-h },
                               (Rectangle){ 0, 0, (float)sw, (float)shh }, (Vector2){ 0, 0 }, 0, WHITE);
            if (physOn && physReady) {   // slime-mould veins glowing over the fractal
                Color pc = { (unsigned char)(90 + 145*promS), (unsigned char)(205 - 80*promS), (unsigned char)(95 - 60*promS), 255 };
                pc.a = (unsigned char)(255*Clamp01(0.30f + 0.5f*pulseFx));
                BeginBlendMode(BLEND_ADDITIVE);
                DrawTexturePro(physTex, (Rectangle){ 0, 0, (float)PHYS_W, (float)PHYS_H },
                               (Rectangle){ 0, 0, (float)sw, (float)shh }, (Vector2){ 0, 0 }, 0, pc);
                EndBlendMode();
            }
        EndTextureMode();
        BeginTextureMode(rtOut);
            {   // visualisation grade: show file (viz_*) x phone remote
                float hue = (showP[SP_ZHUE] + rv[R_ZHUE])*0.0174533f, sat = showP[SP_ZSAT]*rv[R_ZSAT];
                float bright = showP[SP_ZBRIGHT]*rv[R_ZBRIGHT], con = showP[SP_ZCONTRAST]*rv[R_ZCON], zero = 0;
                Vector3 wt = WheelTint(rv[R_ZWX], rv[R_ZWY]);
                Vector3 tint = { showP[SP_ZTINT]*wt.x, showP[SP_ZTINT_G]*wt.y, showP[SP_ZTINT_B]*wt.z };
                SetShaderValue(vizSh, locZHue, &hue, SHADER_UNIFORM_FLOAT);
                SetShaderValue(vizSh, locZSat, &sat, SHADER_UNIFORM_FLOAT);
                SetShaderValue(vizSh, locZBright, &bright, SHADER_UNIFORM_FLOAT);
                SetShaderValue(vizSh, locZCon, &con, SHADER_UNIFORM_FLOAT);
                SetShaderValue(vizSh, locZTint, &tint, SHADER_UNIFORM_VEC3);
                SetShaderValue(vizSh, locZGrain, &zero, SHADER_UNIFORM_FLOAT);
                BeginShaderMode(vizSh);
                DrawTexturePro(rtViz.texture, (Rectangle){ 0, 0, (float)sw, (float)-shh },
                               (Rectangle){ 0, 0, (float)sw, (float)shh }, (Vector2){ 0, 0 }, 0, WHITE);
                EndShaderMode();
            }
            if (sv.active) {
                float op = Clamp01(showP[SP_OPACITY]*gradeV[G_VIDEO]*rv[R_DOP]), t = showV[SV_T];
                Vector3 wt = WheelTint(rv[R_DWX], rv[R_DWY]);
                Vector3 tint = { showP[SP_VTINT]*wt.x, showP[SP_VTINT_G]*wt.y, showP[SP_VTINT_B]*wt.z };
                float vhue = (showP[SP_VHUE] + rv[R_DHUE])*0.0174533f, vsat = showP[SP_VSAT]*rv[R_DSAT];
                float vbright = showP[SP_VBRIGHT]*rv[R_DBRIGHT], vcon = showP[SP_VCONTRAST]*rv[R_DCON];
                SetShaderValue(filmSh, locFilmHue, &vhue, SHADER_UNIFORM_FLOAT);
                SetShaderValue(filmSh, locFilmBright, &vbright, SHADER_UNIFORM_FLOAT);
                SetShaderValue(filmSh, locFilmT, &t, SHADER_UNIFORM_FLOAT);
                SetShaderValue(filmSh, locFilmOp, &op, SHADER_UNIFORM_FLOAT);
                SetShaderValue(filmSh, locFilmMix, &sv.mix, SHADER_UNIFORM_FLOAT);
                SetShaderValue(filmSh, locFilmWarp, &showP[SP_WARP], SHADER_UNIFORM_FLOAT);
                SetShaderValue(filmSh, locFilmZoom, &showP[SP_ZOOM], SHADER_UNIFORM_FLOAT);
                SetShaderValue(filmSh, locFilmSat, &vsat, SHADER_UNIFORM_FLOAT);
                SetShaderValue(filmSh, locFilmCon, &vcon, SHADER_UNIFORM_FLOAT);
                SetShaderValue(filmSh, locFilmTint, &tint, SHADER_UNIFORM_VEC3);
                BeginShaderMode(filmSh); BeginBlendMode(sv.over ? BLEND_ALPHA : BLEND_ADDITIVE);
                SetShaderValueTexture(filmSh, locFilmPrev, vidTex[1]);
                DrawTexturePro(vidTex[0], (Rectangle){ 0, 0, VID_W, VID_H },
                               (Rectangle){ 0, 0, (float)sw, (float)shh }, (Vector2){ 0, 0 }, 0, WHITE);
                EndBlendMode(); EndShaderMode();
            }
        EndTextureMode();

        float grainNow = 0;
        BeginDrawing();
            {   // grade + grain over the whole picture; grain grows with loudness between grain_db_lo and grain_db_hi
                float lo = showP[SP_GRAIN_DB_LO], hi = showP[SP_GRAIN_DB_HI];
                float u = hi > lo ? Clamp01((dbS - lo)/(hi - lo)) : (dbS >= hi);
                u = u*u*(3 - 2*u);
                float grain = fmaxf(0, showP[SP_GRAIN_MIN] + (showP[SP_GRAIN_MAX] - showP[SP_GRAIN_MIN])*u)*gradeV[G_GRAIN]*rv[R_GRAIN];
                float fps = showP[SP_GRAIN_FPS];
                float seed = (float)fmod(fabs(showP[SP_GRAIN_SEED])*31.0 + (fps > 0 ? floor(GetTime()*fps) : 0), 997.0);
                float hue = (showP[SP_HUE] + gradeV[G_HUE])*0.0174533f, sat = showP[SP_SAT]*gradeV[G_SAT];
                float bright = showP[SP_BRIGHT]*gradeV[G_BRIGHT], con = showP[SP_CONTRAST]*gradeV[G_CONTRAST];
                float wm = gradeV[G_WARM], mg = gradeV[G_MAGENTA];
                Vector3 tint = { showP[SP_TINT]*(1 + 0.25f*wm)*(1 + 0.12f*mg), showP[SP_TINT_G]*(1 - 0.22f*mg),
                                 showP[SP_TINT_B]*(1 - 0.25f*wm)*(1 + 0.12f*mg) };
                float gsize = showP[SP_GRAIN_SIZE], gcol = Clamp01(showP[SP_GRAIN_COLOR]);
                SetShaderValue(gradeSh, locGHue, &hue, SHADER_UNIFORM_FLOAT);
                SetShaderValue(gradeSh, locGSat, &sat, SHADER_UNIFORM_FLOAT);
                SetShaderValue(gradeSh, locGBright, &bright, SHADER_UNIFORM_FLOAT);
                SetShaderValue(gradeSh, locGCon, &con, SHADER_UNIFORM_FLOAT);
                SetShaderValue(gradeSh, locGTint, &tint, SHADER_UNIFORM_VEC3);
                SetShaderValue(gradeSh, locGGrain, &grain, SHADER_UNIFORM_FLOAT);
                SetShaderValue(gradeSh, locGSize, &gsize, SHADER_UNIFORM_FLOAT);
                SetShaderValue(gradeSh, locGSeed, &seed, SHADER_UNIFORM_FLOAT);
                SetShaderValue(gradeSh, locGColor, &gcol, SHADER_UNIFORM_FLOAT);
                BeginShaderMode(gradeSh);
                DrawTexturePro(rtOut.texture, (Rectangle){ 0, 0, (float)sw, (float)-shh },
                               (Rectangle){ 0, 0, (float)sw, (float)shh }, (Vector2){ 0, 0 }, 0, WHITE);
                EndShaderMode();
                grainNow = grain;
            }

            // HUD
            if (!hideUi) {
            int x0 = fsz, y0 = fsz;
            DrawRectangle(x0 - fsz/2, y0 - fsz/2, fsz*16, fsz*7, Fade(BLACK, 0.45f));
            if (organism) DrawText(TextFormat("slice %+.2f%s", SLICE_C + SLICE_A*sinf(slice/SLICE_A), kick > 0.5f ? "  *" : ""), x0, y0, fsz, RAYWHITE);
            else DrawText(TextFormat("zoom  1e%.1f%s", depth*0.30103f, ascending ? "  (rising)" : atLimit ? "  (limit)" : ""), x0, y0, fsz, RAYWHITE);
            if (ascending)
                DrawText(colorMode ? TextFormat("limit reached - sing to rise, next: %s", DIVES[dive].name)
                                   : "limit reached - sing to rise",
                         (int)(autoBtn.x + fsz*0.5f), (int)(autoBtn.y + autoBtn.height + fsz*0.5f), fsz, (Color){ 190, 230, 150, 255 });
            DrawText(voiced ? TextFormat("f0    %.0f Hz", f0s) : "f0    --", x0, y0 + fsz*1.3f, fsz, RAYWHITE);
            DrawText(promS > 0.02f ? TextFormat("over  %.0f Hz", otS) : "over  --", x0, y0 + fsz*2.6f, fsz, RAYWHITE);
            DrawRectangle(x0, y0 + fsz*4.0f, (int)(fsz*8*Clamp01(level*8)), fsz/2, GREEN);
            DrawRectangle(x0, y0 + fsz*4.8f, (int)(fsz*8*promS), fsz/2, ORANGE);
            if (!fileData && !capOn) DrawText("waiting for microphone...", x0, y0 + fsz*5.6f, fsz, YELLOW);
            DrawRectangleRec(modeBtn, Fade(BLACK, 0.45f));
            Color mc = organism ? (Color){ 150, 220, 120, 255 } : colorMode ? ORANGE : RAYWHITE;
            DrawRectangleLinesEx(modeBtn, 2, Fade(mc, 0.8f));
            DrawText(TextFormat("%s  [M]", SCENES[scene].name),
                     (int)(modeBtn.x + fsz*0.5f), (int)(modeBtn.y + fsz*0.4f), fsz, mc);
            // auto show: on/off, time to the next scene
            {
                Color ac = autoOn ? (Color){ 150, 220, 120, 255 } : Fade(RAYWHITE, 0.6f);
                DrawRectangleRec(autoBtn, Fade(BLACK, 0.45f));
                DrawRectangleLinesEx(autoBtn, 2, Fade(ac, 0.8f));
                DrawText(autoOn ? TextFormat("auto show: %.0fs  [A]  H: hide", fmaxf(0, sceneSec - sceneT)) : "auto show: off  [A]  H: hide",
                         (int)(autoBtn.x + fsz*0.5f), (int)(autoBtn.y + fsz*0.4f), fsz, ac);
                if (autoOn)
                    DrawRectangle((int)autoBtn.x + 2, (int)(autoBtn.y + autoBtn.height - fsz*0.25f),
                                  (int)((autoBtn.width - 4)*Clamp01(sceneT/sceneSec)), (int)(fsz*0.18f), Fade(ac, 0.8f));
            }
            // physarum toggle
            Color phc = physOn ? (Color){ 150, 220, 120, 255 } : Fade(RAYWHITE, 0.6f);
            DrawRectangleRec(physBtn, Fade(BLACK, 0.45f));
            DrawRectangleLinesEx(physBtn, 2, Fade(phc, 0.8f));
            {
                static const char *NN[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
                const char *t = !physOn ? "physarum: off  [P]"
                              : domNote ? TextFormat("physarum %s%d %.0fHz [P]", NN[domNote % 12], domNote/12 - 1, 440*powf(2, (domNote - 69)/12.0f))
                              : "physarum: on  [P]";
                DrawText(t, (int)(physBtn.x + fsz*0.5f), (int)(physBtn.y + fsz*0.4f), fsz, phc);
                if (physOn)   // chaos meter along the bottom of the button
                    DrawRectangle((int)physBtn.x + 2, (int)(physBtn.y + physBtn.height - fsz*0.25f), (int)((physBtn.width - 4)*chaosU), (int)(fsz*0.18f), Fade((Color){ 235, 125, 35, 255 }, 0.8f));
            }
            // beat: tempo, a dot that flashes on every beat, strength slider
            DrawRectangleRec(beatRow, Fade(BLACK, 0.45f));
            DrawRectangleLinesEx(beatRow, 2, Fade(RAYWHITE, 0.5f));
            DrawCircleV((Vector2){ beatRow.x + fsz*0.9f, beatRow.y + beatRow.height/2 }, fsz*0.35f,
                        Fade((Color){ 235, 125, 35, 255 }, 0.15f + 0.85f*beatEnv));
            DrawText(bt.bpm > 0 && beatOn > 0 ? TextFormat("%3.0f bpm", bt.bpm) : "--- bpm",
                     (int)(beatRow.x + fsz*1.6f), (int)(beatRow.y + fsz*0.4f), fsz, Fade(RAYWHITE, 0.4f + 0.6f*beatOn));
            DrawRectangleRec(beatTrack, Fade(RAYWHITE, 0.25f));
            DrawRectangleRec((Rectangle){ beatTrack.x, beatTrack.y, beatTrack.width*beatAmt/1.5f, beatTrack.height }, Fade(ORANGE, 0.8f));
            DrawText(TextFormat("x%.1f", beatAmt), (int)(beatTrack.x - fsz*2.5f), (int)(beatRow.y + fsz*0.4f), fsz, ORANGE);

            // compass: where the overtone points
            Vector2 c = { (float)(sw - fsz*4), (float)(fsz*4) };
            float r = fsz*2.6f;
            {
                float a = 0.45f + 0.55f*promS, ca = cosf(otAngle), sa = sinf(otAngle);
                DrawCircleV(c, r + fsz*0.3f, Fade(BLACK, 0.6f));
                DrawRing(c, r - 2, r + 1, 0, 360, 48, Fade(RAYWHITE, 0.9f));
                if (promS > 0.02f)    // where the voice pulls right now (unsmoothed)
                    DrawLineEx(c, (Vector2){ c.x + cosf(rawAngle)*r, c.y - sinf(rawAngle)*r }, 2, Fade(RAYWHITE, 0.35f));
                Vector2 tip = { c.x + ca*r*0.92f, c.y - sa*r*0.92f };
                Vector2 base = { c.x + ca*r*0.55f, c.y - sa*r*0.55f };
                float hw = r*0.2f;
                DrawLineEx(c, base, fsz*0.3f, Fade(ORANGE, a));
                DrawTriangle(tip, (Vector2){ base.x - sa*hw, base.y - ca*hw }, (Vector2){ base.x + sa*hw, base.y + ca*hw }, Fade(ORANGE, a));
                DrawTriangle(tip, (Vector2){ base.x + sa*hw, base.y + ca*hw }, (Vector2){ base.x - sa*hw, base.y - ca*hw }, Fade(ORANGE, a));
            }
            DrawCircleV(c, fsz*0.2f, RAYWHITE);

            // spectrum: faint curve + harmonic comb, the dominant overtone in orange
            {
                const float RANGE = 40;                          // dB shown
                int sfs = fsz*6/10 > 10 ? fsz*6/10 : 10;
                float pw = sw*0.5f < fsz*26 ? sw*0.5f : fsz*26, ph = fsz*6.5f;
                float px = sw - fsz - pw, py = track.y - fsz*2.2f - ph;
                float ax = px + fsz*0.5f, aw = pw - fsz, ay = py + ph - sfs*1.6f, ah = ay - (py + fsz*1.6f);
                float top = -200;
                for (int b = 0; b < VOICE_SPEC_BINS; b++) if (specS[b] > top) top = specS[b];
                specRef = top > specRef - 0.3f ? top : specRef - 0.3f;   // slow fall, no jumping scale
                #define SX(f) (ax + aw*(f)/VOICE_SPEC_FMAX)
                #define SY(db) (ay - ah*Clamp01(((db) - (specRef - RANGE))/RANGE))

                DrawRectangleRec((Rectangle){ px, py, pw, ph }, Fade(BLACK, 0.5f));
                DrawRectangleRec((Rectangle){ SX(VOICE_OT_FMIN), py + fsz*1.4f, SX(VOICE_SPEC_FMAX) - SX(VOICE_OT_FMIN), ay - py - fsz*1.4f }, Fade(ORANGE, 0.07f));
                DrawLineEx((Vector2){ ax, ay }, (Vector2){ ax + aw, ay }, 1, Fade(RAYWHITE, 0.5f));
                for (int k = 1; k < 4; k++) {
                    float x = SX(k*1000.0f);
                    DrawLineEx((Vector2){ x, ay }, (Vector2){ x, ay - ah }, 1, Fade(RAYWHITE, 0.12f));   // 1/2/3 kHz grid
                }
                for (int b = 1; b < VOICE_SPEC_BINS; b++) {
                    float f0b = (b - 0.5f)*VOICE_SPEC_FMAX/VOICE_SPEC_BINS, f1b = (b + 0.5f)*VOICE_SPEC_FMAX/VOICE_SPEC_BINS;
                    DrawLineEx((Vector2){ SX(f0b), SY(specS[b-1]) }, (Vector2){ SX(f1b), SY(specS[b]) }, 1.5f, Fade(SKYBLUE, 0.45f));
                }

                if (harmVis > 0.03f && f0s > 0) {
                    float spacing = SX(f0s) - SX(0);
                    float bw = spacing*0.4f; if (bw < 2) bw = 2; if (bw > fsz*0.8f) bw = fsz*0.8f;
                    int step = 1; while (spacing*step < sfs*1.4f) step++;
                    int hl = promS > 0.02f ? otN : 0;
                    for (int n = 1; n <= nHarmS; n++) {
                        float x = SX(n*f0s), y = SY(harmS[n-1]);
                        Color c = n == hl ? ORANGE : n == 1 ? RAYWHITE : LIGHTGRAY;
                        float a = harmVis*(n == hl ? 0.5f + 0.5f*promS : n == 1 ? 0.9f : hl ? 0.35f : 0.55f);
                        if (n == hl) DrawRectangleRec((Rectangle){ x - bw*1.5f, y, bw*3, ay - y }, Fade(ORANGE, 0.25f*a));
                        DrawRectangleRec((Rectangle){ x - bw/2, y, bw, ay - y }, Fade(c, a));
                        if (n == hl || n % step == 0 || n == 1)
                            DrawText(TextFormat("%d", n), (int)(x - MeasureText(TextFormat("%d", n), sfs)/2), (int)(ay + sfs*0.3f), sfs, Fade(n == hl ? ORANGE : RAYWHITE, n == hl ? harmVis : 0.6f*harmVis));
                    }
                    if (hl) {
                        const char *t = TextFormat("H%d  %.0f Hz", hl, hl*f0s);
                        int tw = MeasureText(t, fsz);
                        float tx = SX(hl*f0s) - tw/2.0f;
                        if (tx < px + fsz*0.3f) tx = px + fsz*0.3f;
                        if (tx + tw > px + pw - fsz*0.3f) tx = px + pw - fsz*0.3f - tw;
                        DrawText(t, (int)tx, (int)(py + fsz*0.3f), fsz, Fade(ORANGE, harmVis));
                    }
                }
                if (!(harmVis > 0.03f && f0s > 0 && promS > 0.02f))
                    DrawText("spectrum", (int)(px + fsz*0.3f), (int)(py + fsz*0.3f), fsz, Fade(RAYWHITE, 0.5f));
                #undef SX
                #undef SY
            }

            // base-tone slider: blue = zoom out, green = zoom in, white tick = current f0
            {
                float lr = logf(F0_HI/F0_LO);
                float bx = track.x + track.width*SliderU(f0Base);
                float dz = f0Base > 0 ? track.width*(1 - ZERO_U)*(DEAD_SEMI/12*logf(2))/lr : 0;
                DrawRectangle((int)(hit.x), (int)(track.y - fsz*1.6f), (int)hit.width, (int)(fsz*3.0f), Fade(BLACK, 0.35f));
                DrawRectangleRec((Rectangle){ track.x, track.y, bx - dz - track.x, track.height }, Fade(SKYBLUE, 0.8f));
                DrawRectangleRec((Rectangle){ bx - dz, track.y, 2*dz, track.height }, Fade(GRAY, 0.8f));
                DrawRectangleRec((Rectangle){ bx + dz, track.y, track.x + track.width - bx - dz, track.height }, Fade(LIME, 0.8f));
                if (voiced && f0s > 0) {
                    float fx = track.x + track.width*SliderU(f0s);
                    DrawRectangle((int)fx - 2, (int)(track.y - fsz*0.4f), 4, (int)(track.height + fsz*0.8f), RAYWHITE);
                }
                DrawCircleV((Vector2){ bx, track.y + track.height/2 }, fsz*0.6f, sliderDrag ? YELLOW : RAYWHITE);
                const char *lbl = f0Base > 0 ? TextFormat("base %.0f Hz", f0Base) : "base 0 Hz: any voice = +, louder = faster";
                int lw = MeasureText(lbl, fsz);
                float lx = bx - lw/2.0f;
                if (lx < hit.x) lx = hit.x;
                if (lx + lw > hit.x + hit.width) lx = hit.x + hit.width - lw;
                DrawText(lbl, (int)lx, (int)(track.y - fsz*1.5f), fsz, RAYWHITE);
                DrawText("-", (int)(track.x - fsz*1.4f), (int)(track.y - fsz*0.3f), fsz, SKYBLUE);
                DrawText("+", (int)(track.x + track.width + fsz*0.6f), (int)(track.y - fsz*0.3f), fsz, LIME);
            }
            if (gradeUi) {   // palette sliders + video / grain readout
                DrawRectangleRec(gradeBox, Fade(BLACK, 0.6f));
                DrawRectangleLinesEx(gradeBox, 2, Fade(RAYWHITE, 0.5f));
                DrawText(sv.active ? TextFormat("video: %s (%d)  [V] [,][.]   grain %.2f", sv.name, ShowClipCount(), grainNow)
                                   : TextFormat("video: off (%d clips)  [V]   grain %.2f", ShowClipCount(), grainNow),
                         (int)(gradeBox.x + fsz*0.5f), (int)(gradeBox.y + fsz*0.4f), (int)(fsz*0.8f), Fade(RAYWHITE, 0.8f));
                for (int i = 0; i < N_GRADE; i++) {
                    float ry = gradeBox.y + fsz*(1.6f + 1.4f*i);
                    Rectangle tr = { gradeBox.x + fsz*8, ry + fsz*0.5f, gradeBox.width - fsz*11, fsz*0.4f };
                    float u = (gradeV[i] - GRADE_UI[i].lo)/(GRADE_UI[i].hi - GRADE_UI[i].lo);
                    float u0 = (GRADE_UI[i].def - GRADE_UI[i].lo)/(GRADE_UI[i].hi - GRADE_UI[i].lo);
                    Color gc = i == gradeDrag ? ORANGE : (Color){ 190, 230, 150, 255 };
                    DrawText(GRADE_UI[i].name, (int)(gradeBox.x + fsz*0.5f), (int)(ry + fsz*0.3f), (int)(fsz*0.8f), RAYWHITE);
                    DrawRectangleRec(tr, Fade(RAYWHITE, 0.2f));
                    DrawRectangle((int)(tr.x + tr.width*u0) - 1, (int)(tr.y - fsz*0.15f), 2, (int)(tr.height + fsz*0.3f), Fade(RAYWHITE, 0.5f));
                    DrawCircleV((Vector2){ tr.x + tr.width*u, tr.y + tr.height/2 }, fsz*0.35f, gc);
                    DrawText(i == G_HUE ? TextFormat("%+.0f", gradeV[i]) : TextFormat("%.2f", gradeV[i]),
                             (int)(tr.x + tr.width + fsz*0.6f), (int)(ry + fsz*0.3f), (int)(fsz*0.8f), gc);
                }
            }
            }   // !hideUi
        EndDrawing();

        if (++stateTick % 6 == 0) {
            char js[4096]; int n = 0;
            #define J(...) n += snprintf(js + n, n < (int)sizeof js ? sizeof js - n : 0, __VA_ARGS__)
            J("{\"scene\":%d,\"scenes\":[", scene);
            for (int i = 0, first = 1; i < N_SCENES; i++)
                if (SceneOn(i)) { J("%s[%d,\"%s\"]", first ? "" : ",", i, SCENES[i].name); first = 0; }
            J("],\"phys\":%d,\"video\":%d,\"auto\":%d,\"hide\":%d,\"next\":%.0f,", physOn, ShowVideoOn(), autoOn, hideUi,
              fmaxf(0, sceneSec - sceneT));
            J("\"clip\":\"");
            for (const char *c = sv.active && sv.name ? sv.name : ""; *c && n < (int)sizeof js - 600; c++)
                if (*c != '"' && *c != '\\' && (unsigned char)*c >= 32) js[n++] = *c;
            J("\",\"clips\":%d,\"bpm\":%.0f,\"level\":%.3f,\"chaos\":%.2f,\"vals\":{", ShowClipCount(),
              bt.bpm > 0 && beatOn > 0 ? bt.bpm : 0, Clamp01(level*8), chaosU);
            for (int i = 0; i < N_RV; i++) J("%s\"%s\":[%g,%g,%g,%g]", i ? "," : "", RV[i].key, rv[i], RV[i].lo, RV[i].hi, RV[i].def);
            J("}}");
            #undef J
            if (n < (int)sizeof js) RemoteSetState(js);
        }

        if (shotDir && GetTime() - startTime > shotNext) {
            TakeScreenshot(TextFormat("%s/shot_%05.1f.png", shotDir, shotNext));
            TraceLog(LOG_INFO, "SHOT t=%.1f f0=%.0f ot=%.0f prom=%.2f zoom=%.2f scale=%.3e c=(%.12f,%.12f) level=%g db=%.1f pulse=%g kick=%g op=%g warp=%g",
                     shotNext, f0s, otS, promS, zoomRate, scale, cx, cy, level, dbS, pulseFx, kick, showP[SP_OPACITY], showP[SP_WARP]);
            shotNext += 0.5;
            if (fileData && !getenv("VOICE_LOOP") && filePos >= fileWave.frameCount && shotNext > fileWave.frameCount/sampleRate + 1.5) break;
        }
    }

    ShowShutdown();
    if (capOn) ma_device_uninit(&capDev);
    if (fileWave.data) UnloadWave(fileWave);
    UnloadRenderTexture(rt);
    UnloadTexture(refTex);
    UnloadShader(sh);
    CloseWindow();
    return 0;
}
