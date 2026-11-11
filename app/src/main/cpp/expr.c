// Recursive-descent parser into a flat node array, evaluated by walking the tree.
//   expr  = term  (('+' | '-') term)*
//   term  = unary (('*' | '/' | '%') unary)*
//   unary = '-' unary | pow
//   pow   = atom ('^' unary)?
//   atom  = number | name | name '(' expr (',' expr)* ')' | '(' expr ')'
#include "expr.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { N_NUM, N_VAR, N_NEG, N_ADD, N_SUB, N_MUL, N_DIV, N_MOD, N_POW, N_FUNC };
enum { F_SIN, F_COS, F_TAN, F_ABS, F_MIN, F_MAX, F_CLAMP, F_MIX, F_POW, F_SQRT, F_EXP, F_LOG,
       F_FLOOR, F_FRACT, F_STEP, F_SMOOTH, F_NOISE };
static const struct { const char *name; int args; } FUNCS[] = {
    { "sin", 1 }, { "cos", 1 }, { "tan", 1 }, { "abs", 1 }, { "min", 2 }, { "max", 2 }, { "clamp", 3 },
    { "mix", 3 }, { "pow", 2 }, { "sqrt", 1 }, { "exp", 1 }, { "log", 1 }, { "floor", 1 }, { "fract", 1 },
    { "step", 2 }, { "smoothstep", 3 }, { "noise", 1 },
};
#define N_FUNCS ((int)(sizeof FUNCS/sizeof FUNCS[0]))
#define MAX_NODES 256

typedef struct { int op, fn, a[3]; float v; } Node;
struct Expr { int n, root; Node nodes[MAX_NODES]; };

typedef struct {
    const char *s;
    const char *const *names; int nNames;
    Expr *e;
    char *err; int errLen; int failed;
} P;

static int Fail(P *p, const char *msg)
{
    if (!p->failed) snprintf(p->err, p->errLen, "%s at \"%.12s\"", msg, *p->s ? p->s : "<end>");
    p->failed = 1;
    return -1;
}
static void Skip(P *p) { while (isspace((unsigned char)*p->s)) p->s++; }
static int Add(P *p, int op, int a, int b, float v)
{
    if (p->failed) return -1;
    if (p->e->n >= MAX_NODES) return Fail(p, "too long");
    Node *n = &p->e->nodes[p->e->n];
    n->op = op; n->fn = 0; n->a[0] = a; n->a[1] = b; n->a[2] = -1; n->v = v;
    return p->e->n++;
}

static int Expr_(P *p);
static int Unary(P *p);

static int Atom(P *p)
{
    Skip(p);
    if (p->failed) return -1;
    if (*p->s == '(') {
        p->s++;
        int r = Expr_(p);
        Skip(p);
        if (*p->s != ')') return Fail(p, "expected )");
        p->s++;
        return r;
    }
    if (isdigit((unsigned char)*p->s) || *p->s == '.') {
        char *end;
        float v = strtof(p->s, &end);
        if (end == p->s) return Fail(p, "bad number");
        p->s = end;
        return Add(p, N_NUM, -1, -1, v);
    }
    if (isalpha((unsigned char)*p->s) || *p->s == '_') {
        const char *b = p->s;
        while (isalnum((unsigned char)*p->s) || *p->s == '_') p->s++;
        int len = (int)(p->s - b);
        Skip(p);
        if (*p->s == '(') {
            int f = -1;
            for (int i = 0; i < N_FUNCS; i++)
                if ((int)strlen(FUNCS[i].name) == len && !strncmp(FUNCS[i].name, b, len)) f = i;
            if (f < 0) { p->s = b; return Fail(p, "unknown function"); }
            p->s++;
            int args[3] = { -1, -1, -1 };
            for (int k = 0; k < FUNCS[f].args; k++) {
                if (k) { Skip(p); if (*p->s != ',') return Fail(p, "expected ,"); p->s++; }
                args[k] = Expr_(p);
            }
            Skip(p);
            if (*p->s != ')') return Fail(p, "expected )");
            p->s++;
            int r = Add(p, N_FUNC, args[0], args[1], 0);
            if (r >= 0) { p->e->nodes[r].fn = f; p->e->nodes[r].a[2] = args[2]; }
            return r;
        }
        if (len == 2 && !strncmp(b, "pi", 2)) return Add(p, N_NUM, -1, -1, 3.14159265f);
        for (int i = 0; i < p->nNames; i++)
            if ((int)strlen(p->names[i]) == len && !strncmp(p->names[i], b, len)) return Add(p, N_VAR, -1, -1, (float)i);
        p->s = b;
        return Fail(p, "unknown name");
    }
    return Fail(p, "unexpected");
}

static int Pow(P *p)
{
    int a = Atom(p);
    Skip(p);
    if (*p->s == '^') { p->s++; return Add(p, N_POW, a, Unary(p), 0); }
    return a;
}

static int Unary(P *p)
{
    Skip(p);
    if (*p->s == '-') { p->s++; return Add(p, N_NEG, Unary(p), -1, 0); }
    if (*p->s == '+') { p->s++; return Unary(p); }
    return Pow(p);
}

static int Term(P *p)
{
    int a = Unary(p);
    for (;;) {
        Skip(p);
        int op = *p->s == '*' ? N_MUL : *p->s == '/' ? N_DIV : *p->s == '%' ? N_MOD : -1;
        if (op < 0 || p->failed) return a;
        p->s++;
        a = Add(p, op, a, Unary(p), 0);
    }
}

static int Expr_(P *p)
{
    int a = Term(p);
    for (;;) {
        Skip(p);
        int op = *p->s == '+' ? N_ADD : *p->s == '-' ? N_SUB : -1;
        if (op < 0 || p->failed) return a;
        p->s++;
        a = Add(p, op, a, Term(p), 0);
    }
}

Expr *ExprParse(const char *src, const char *const *names, int nNames, char *err, int errLen)
{
    P p = { src, names, nNames, calloc(1, sizeof(Expr)), err, errLen, 0 };
    p.e->root = Expr_(&p);
    Skip(&p);
    if (!p.failed && *p.s) Fail(&p, "unexpected");
    if (p.failed) { free(p.e); return NULL; }
    return p.e;
}

void ExprFree(Expr *e) { free(e); }

// smooth 1D value noise in -1..1
static float Hash(float i) { float h = sinf(i*127.1f)*43758.5453f; return 2*(h - floorf(h)) - 1; }
static float Noise(float x)
{
    float i = floorf(x), f = x - i;
    f = f*f*(3 - 2*f);
    return Hash(i) + (Hash(i + 1) - Hash(i))*f;
}

static float Eval(const Expr *e, int i, const float *vars)
{
    const Node *n = &e->nodes[i];
    float a = n->a[0] >= 0 ? Eval(e, n->a[0], vars) : 0;
    float b = n->a[1] >= 0 ? Eval(e, n->a[1], vars) : 0;
    switch (n->op) {
    case N_NUM: return n->v;
    case N_VAR: return vars[(int)n->v];
    case N_NEG: return -a;
    case N_ADD: return a + b;
    case N_SUB: return a - b;
    case N_MUL: return a*b;
    case N_DIV: return b != 0 ? a/b : 0;
    case N_MOD: return b != 0 ? a - b*floorf(a/b) : 0;
    case N_POW: return powf(a, b);
    }
    float c = n->a[2] >= 0 ? Eval(e, n->a[2], vars) : 0;
    switch (n->fn) {
    case F_SIN: return sinf(a);
    case F_COS: return cosf(a);
    case F_TAN: return tanf(a);
    case F_ABS: return fabsf(a);
    case F_MIN: return fminf(a, b);
    case F_MAX: return fmaxf(a, b);
    case F_CLAMP: return fminf(fmaxf(a, b), c);
    case F_MIX: return a + (b - a)*c;
    case F_POW: return powf(a, b);
    case F_SQRT: return a > 0 ? sqrtf(a) : 0;
    case F_EXP: return expf(a);
    case F_LOG: return a > 0 ? logf(a) : 0;
    case F_FLOOR: return floorf(a);
    case F_FRACT: return a - floorf(a);
    case F_STEP: return b < a ? 0.0f : 1.0f;
    case F_SMOOTH: { float t = fminf(fmaxf((c - a)/(b - a), 0), 1); return t*t*(3 - 2*t); }
    case F_NOISE: return Noise(a);
    }
    return 0;
}

float ExprEval(const Expr *e, const float *vars)
{
    float v = Eval(e, e->root, vars);
    return isfinite(v) ? v : 0;
}
