#include "physarum.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct { float x, y, a; } Agent;

static int W, H, N;
static Agent *ag;
static float *trail, *tmp;
static unsigned char *out8;
static unsigned rng = 1;

static unsigned Rand(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static float Rand01(void) { return (Rand() & 0xffffff)/16777216.0f; }

void PhysInit(int w, int h, int agents, unsigned seed)
{
    W = w; H = h; N = agents; rng = seed ? seed : 1;
    free(ag); free(trail); free(tmp); free(out8);
    ag = malloc(sizeof *ag*N);
    trail = calloc((size_t)W*H, sizeof *trail);
    tmp = calloc((size_t)W*H, sizeof *tmp);
    out8 = calloc((size_t)W*H, 1);
    // scattered start: a symmetric start collapses into a few straight trunks along the axes
    for (int i = 0; i < N; i++) { ag[i].x = Rand01()*W; ag[i].y = Rand01()*H; ag[i].a = Rand01()*6.2831853f; }
}

static float Sense(float x, float y)
{
    int ix = (int)x, iy = (int)y;
    ix %= W; if (ix < 0) ix += W;
    iy %= H; if (iy < 0) iy += H;
    return trail[iy*W + ix];
}

void PhysStep(PhysParams p)
{
    if (!ag) return;
    int nWild = (int)(p.wildFrac*N);
    float kx = 6.2831853f*p.swirlScale/H, ky = kx*1.3f, t1 = p.time*0.7f, t2 = p.time*0.53f;
    for (int i = 0; i < N; i++) {
        Agent *g = &ag[i];
        if (p.jolt > 0 && Rand01() < p.jolt) g->a = Rand01()*6.2831853f;
        // flowing vortex field bends everyone
        if (p.swirl > 0) g->a += p.swirl*sinf(g->x*kx + t1)*cosf(g->y*ky - t2);
        if (i < nWild) {   // wild: no sensing, jittery and fast, bright trace
            g->a += (Rand01() - 0.5f)*(1.2f + 2.0f*p.wander);
            g->x += cosf(g->a)*p.speed*2.2f; g->y += sinf(g->a)*p.speed*2.2f;
            if (g->x < 0) g->x += W; else if (g->x >= W) g->x -= W;
            if (g->y < 0) g->y += H; else if (g->y >= H) g->y -= H;
            trail[(int)g->y*W + (int)g->x] += p.deposit*3.0f;
            continue;
        }
        float f  = Sense(g->x + cosf(g->a)*p.sensorDist, g->y + sinf(g->a)*p.sensorDist);
        float fl = Sense(g->x + cosf(g->a + p.sensorAngle)*p.sensorDist, g->y + sinf(g->a + p.sensorAngle)*p.sensorDist);
        float fr = Sense(g->x + cosf(g->a - p.sensorAngle)*p.sensorDist, g->y + sinf(g->a - p.sensorAngle)*p.sensorDist);
        g->a += (Rand01() - 0.5f)*(0.3f*p.turn + p.wander);   // wander: keeps it alive, chaos makes it twitch
        if (f >= fl && f >= fr) {}
        else if (f < fl && f < fr) g->a += (Rand() & 1 ? 1 : -1)*p.turn;
        else if (fl > fr) g->a += p.turn;
        else g->a -= p.turn;
        g->x += cosf(g->a)*p.speed; g->y += sinf(g->a)*p.speed;
        if (g->x < 0) g->x += W; else if (g->x >= W) g->x -= W;
        if (g->y < 0) g->y += H; else if (g->y >= H) g->y -= H;
        trail[(int)g->y*W + (int)g->x] += p.deposit;
    }
    // 3x3 diffuse + decay (torus)
    for (int y = 0; y < H; y++) {
        const float *r0 = trail + ((y + H - 1) % H)*W, *r1 = trail + y*W, *r2 = trail + ((y + 1) % H)*W;
        for (int x = 0; x < W; x++) {
            int xl = x ? x - 1 : W - 1, xr = x + 1 < W ? x + 1 : 0;
            float s = r0[xl] + r0[x] + r0[xr] + r1[xl] + r1[x] + r1[xr] + r2[xl] + r2[x] + r2[xr];
            tmp[y*W + x] = (0.4f*r1[x] + 0.6f*s*(1.0f/9))*p.decay;   // partial blur: thinner veins
        }
    }
    float *t = trail; trail = tmp; tmp = t;
}

const unsigned char *PhysTrail8(void)
{
    // normalise by the mean trail: brightness stays the same whatever the density/deposit, cores glow,
    // the halo falls off (squared) instead of flooding the screen
    double sum = 0;
    for (int i = 0; i < W*H; i++) sum += trail[i];
    float k = 1.0f/(float)(3.0*sum/(W*H) + 1e-6);
    for (int i = 0; i < W*H; i++) {
        float v = 1.0f - expf(-trail[i]*k);
        out8[i] = (unsigned char)(v*v*255.0f);
    }
    return out8;
}

int PhysW(void) { return W; }
int PhysH(void) { return H; }
