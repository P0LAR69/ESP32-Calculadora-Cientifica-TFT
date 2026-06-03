// SPDX-License-Identifier: Zlib
/*
 * TINYEXPR COMPLEX MEJORADA v2.0
 *
 * CAMBIOS vs. original:
 *  1. Eliminadas declaraciones dobles de poly_eval / poly_eval_deriv.
 *  2. poly_deflate_conj: corregido índice c[i+2]→c[i] (bug crítico).
 *  3. Newton-Raphson: reintentos con semillas nuevas, convergencia más robusta.
 *  4. c_pow:  manejo de base==0.
 *  5. c_ln:   manejo de |z|==0.
 *  6. c_tanh: protección contra overflow cuando Re(z) > 20.
 *  7. c_fac:  límite y desbordamiento gestionados.
 *  8. Nuevas funciones: asinh, acosh, erf, erfc, gamma (Lanczos),
 *     log2, sign, mod, round, min2, max2.
 *  9. te_interp_vars: variante que acepta tabla de variables extra.
 * 10. Comentarios exhaustivos en español.
 */

#include "tinyexpr_complex_mejorada.h"
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <ctype.h>

/* ────────────────────────────────────────────────────────
 *  Constantes portables
 * ──────────────────────────────────────────────────────── */
#ifndef M_PI
#  define M_PI   3.14159265358979323846
#endif
#ifndef M_PI_2
#  define M_PI_2 1.57079632679489661923
#endif
#ifndef M_E
#  define M_E    2.71828182845904523536
#endif
#ifndef NAN
#  define NAN      (0.0/0.0)
#endif
#ifndef INFINITY
#  define INFINITY (1.0/0.0)
#endif

/* ────────────────────────────────────────────────────────
 *  Tipos de función interna
 * ──────────────────────────────────────────────────────── */
typedef te_complex (*te_fun0)(void);
typedef te_complex (*te_fun1)(te_complex);
typedef te_complex (*te_fun2)(te_complex, te_complex);

/* ────────────────────────────────────────────────────────
 *  Tokens del parser
 * ──────────────────────────────────────────────────────── */
enum {
    TOK_NULL = TE_CLOSURE7 + 1, TOK_ERROR, TOK_END, TOK_SEP,
    TOK_OPEN, TOK_CLOSE, TOK_NUMBER, TOK_VARIABLE, TOK_INFIX
};
enum { TE_CONSTANT = 1 };

/* ────────────────────────────────────────────────────────
 *  Estado del parser
 * ──────────────────────────────────────────────────────── */
typedef struct state {
    const char *start;
    const char *next;
    int type;
    union {
        te_complex      value;
        const te_complex *bound;
        const void      *function;
    };
    void *context;
    const te_variable *lookup;
    int lookup_len;
} state;

/* ────────────────────────────────────────────────────────
 *  Macros auxiliares
 * ──────────────────────────────────────────────────────── */
#define TYPE_MASK(T)   ((T) & 0x1F)
#define IS_PURE(T)     (((T) & TE_FLAG_PURE) != 0)
#define IS_FUNCTION(T) (((T) & TE_FUNCTION0) != 0)
#define IS_CLOSURE(T)  (((T) & TE_CLOSURE0)  != 0)
#define ARITY(T)       (IS_FUNCTION(T)||IS_CLOSURE(T) ? ((T)&7) : 0)

/* Macro de guarda NULL: ejecuta cleanup y retorna NULL. */
#define GUARD_NULL(ptr, ...) do { if (!(ptr)) { __VA_ARGS__; return NULL; } } while(0)

/* ════════════════════════════════════════════════════════
 *  ARITMÉTICA COMPLEJA
 * ════════════════════════════════════════════════════════ */

te_complex te_complex_make(double r, double i) {
    te_complex z; z.real = r; z.imag = i; return z;
}

double te_complex_abs(te_complex z) {
    /* hypot evita overflow para valores grandes */
    return hypot(z.real, z.imag);
}

double te_complex_arg(te_complex z) {
    return atan2(z.imag, z.real);
}

static te_complex c_add(te_complex a, te_complex b) {
    return (te_complex){ a.real + b.real, a.imag + b.imag };
}
static te_complex c_sub(te_complex a, te_complex b) {
    return (te_complex){ a.real - b.real, a.imag - b.imag };
}
static te_complex c_mul(te_complex a, te_complex b) {
    return (te_complex){
        a.real*b.real - a.imag*b.imag,
        a.real*b.imag + a.imag*b.real
    };
}
static te_complex c_div(te_complex a, te_complex b) {
    double d = b.real*b.real + b.imag*b.imag;
    if (d < 1e-300) return (te_complex){ NAN, NAN };
    return (te_complex){
        (a.real*b.real + a.imag*b.imag) / d,
        (a.imag*b.real - a.real*b.imag) / d
    };
}
static te_complex c_neg(te_complex a) {
    return (te_complex){ -a.real, -a.imag };
}
static te_complex c_conj(te_complex a) {
    return (te_complex){ a.real, -a.imag };
}

/* sqrt compleja: módulo-argumento */
static te_complex c_sqrt(te_complex a) {
    double r  = te_complex_abs(a);
    double sr = sqrt(r);
    double th = te_complex_arg(a) * 0.5;
    return (te_complex){ sr * cos(th), sr * sin(th) };
}

/* exp compleja */
static te_complex c_exp(te_complex a) {
    double ex = exp(a.real);
    return (te_complex){ ex * cos(a.imag), ex * sin(a.imag) };
}

/* ln compleja: ln|z| + i·arg(z).  Maneja z=0. */
static te_complex c_ln(te_complex a) {
    double r = te_complex_abs(a);
    if (r < 1e-300) return (te_complex){ -INFINITY, 0.0 };
    return (te_complex){ log(r), te_complex_arg(a) };
}

/* pow compleja: a^b = exp(b·ln(a)).  Maneja a=0. */
static te_complex c_pow(te_complex a, te_complex b) {
    /* 0^x */
    if (a.real == 0.0 && a.imag == 0.0) {
        if (b.real > 0.0 && b.imag == 0.0) return (te_complex){ 0.0, 0.0 };
        if (b.real == 0.0 && b.imag == 0.0) return (te_complex){ 1.0, 0.0 }; /* 0^0 = 1 por conv. */
        return (te_complex){ NAN, NAN };
    }
    /* b puramente real: fórmula simplificada evita NaN innecesarios */
    if (b.imag == 0.0) {
        double r  = pow(te_complex_abs(a), b.real);
        double th = te_complex_arg(a) * b.real;
        return (te_complex){ r*cos(th), r*sin(th) };
    }
    /* Caso general: exp(b·ln(a)) */
    te_complex lna = c_ln(a);
    te_complex blna = c_mul(b, lna);
    return c_exp(blna);
}

/* ── Trigonométricas ── */
static te_complex c_sin(te_complex z) {
    return (te_complex){ sin(z.real)*cosh(z.imag), cos(z.real)*sinh(z.imag) };
}
static te_complex c_cos(te_complex z) {
    return (te_complex){ cos(z.real)*cosh(z.imag), -sin(z.real)*sinh(z.imag) };
}
static te_complex c_tan(te_complex z) { return c_div(c_sin(z), c_cos(z)); }

/* ── Trigonométricas inversas ── */
static te_complex c_asin(te_complex z) {
    /* asin(z) = -i · ln(iz + sqrt(1-z²)) */
    te_complex iz   = { -z.imag, z.real };
    te_complex one  = { 1.0, 0.0 };
    te_complex z2   = c_mul(z, z);
    te_complex sq   = c_sqrt(c_sub(one, z2));
    te_complex ln_v = c_ln(c_add(iz, sq));
    return (te_complex){ ln_v.imag, -ln_v.real };
}
static te_complex c_acos(te_complex z) {
    te_complex r = c_asin(z);
    return (te_complex){ M_PI_2 - r.real, -r.imag };
}
static te_complex c_atan(te_complex z) {
    double x = z.real, y = z.imag;
    return (te_complex){
        0.5 * atan2(2.0*x, 1.0 - x*x - y*y),
        0.25 * log(((x*x) + (y+1)*(y+1)) / ((x*x) + (y-1)*(y-1)))
    };
}
static te_complex c_atan2(te_complex y, te_complex x) {
    /* Solo partes reales (atan2 real) */
    return (te_complex){ atan2(y.real, x.real), 0.0 };
}

/* ── Hiperbólicas ── */
static te_complex c_sinh(te_complex z) {
    return (te_complex){ sinh(z.real)*cos(z.imag), cosh(z.real)*sin(z.imag) };
}
static te_complex c_cosh(te_complex z) {
    return (te_complex){ cosh(z.real)*cos(z.imag), sinh(z.real)*sin(z.imag) };
}
/* tanh: protección contra overflow cuando |Re(z)| grande */
static te_complex c_tanh(te_complex z) {
    if (z.real >  20.0) return (te_complex){  1.0, 0.0 };
    if (z.real < -20.0) return (te_complex){ -1.0, 0.0 };
    return c_div(c_sinh(z), c_cosh(z));
}

/* ── Hiperbólicas inversas ── */
static te_complex c_asinh(te_complex z) {
    /* asinh(z) = ln(z + sqrt(z²+1)) */
    te_complex z2 = c_mul(z, z);
    te_complex one = { 1.0, 0.0 };
    return c_ln(c_add(z, c_sqrt(c_add(z2, one))));
}
static te_complex c_acosh(te_complex z) {
    /* acosh(z) = ln(z + sqrt(z²-1)) */
    te_complex z2  = c_mul(z, z);
    te_complex one = { 1.0, 0.0 };
    return c_ln(c_add(z, c_sqrt(c_sub(z2, one))));
}
static te_complex c_atanh(te_complex z) {
    double x = z.real, y = z.imag;
    return (te_complex){
        0.25 * log(((x+1)*(x+1)+y*y) / ((x-1)*(x-1)+y*y)),
        0.5  * atan2(2.0*y, 1.0 - x*x - y*y)
    };
}

/* ── Logaritmos ── */
static te_complex c_log10(te_complex a) {
    double ln10 = log(10.0);
    te_complex l = c_ln(a);
    return (te_complex){ l.real/ln10, l.imag/ln10 };
}
static te_complex c_log2(te_complex a) {
    double ln2 = log(2.0);
    te_complex l = c_ln(a);
    return (te_complex){ l.real/ln2, l.imag/ln2 };
}

/* ── Redondeo ── */
static te_complex c_ceil (te_complex a) { return (te_complex){ ceil (a.real), 0.0 }; }
static te_complex c_floor(te_complex a) { return (te_complex){ floor(a.real), 0.0 }; }
static te_complex c_round(te_complex a) { return (te_complex){ round(a.real), 0.0 }; }

/* ── Valor absoluto / argumento ── */
static te_complex c_abs_ret(te_complex a) { return (te_complex){ te_complex_abs(a), 0.0 }; }
static te_complex c_arg_ret(te_complex a) { return (te_complex){ te_complex_arg(a), 0.0 }; }
static te_complex c_real(te_complex a)    { return (te_complex){ a.real, 0.0 }; }
static te_complex c_imag(te_complex a)    { return (te_complex){ a.imag, 0.0 }; }
static te_complex c_sign(te_complex a) {
    if (a.real > 0.0) return (te_complex){ 1.0, 0.0 };
    if (a.real < 0.0) return (te_complex){-1.0, 0.0 };
    return (te_complex){ 0.0, 0.0 };
}
static te_complex c_min2(te_complex a, te_complex b) {
    return (te_complex){ fmin(a.real, b.real), 0.0 };
}
static te_complex c_max2(te_complex a, te_complex b) {
    return (te_complex){ fmax(a.real, b.real), 0.0 };
}
static te_complex c_mod(te_complex a, te_complex b) {
    if (b.real == 0.0) return (te_complex){ NAN, 0.0 };
    return (te_complex){ fmod(a.real, b.real), 0.0 };
}

/* ── Combinatoria ── */
static te_complex c_fac(te_complex a) {
    int n = (int)a.real;
    if (n < 0 || n > 170) return (te_complex){ NAN, 0.0 };
    double r = 1.0;
    for (int i = 2; i <= n; i++) r *= i;
    return (te_complex){ r, 0.0 };
}
static te_complex c_ncr(te_complex n, te_complex r) {
    int ni = (int)n.real, ri = (int)r.real;
    if (ri < 0 || ri > ni || ni < 0) return (te_complex){ NAN, 0.0 };
    double res = 1.0;
    for (int i = 1; i <= ri; i++) res = res * (ni - ri + i) / i;
    return (te_complex){ res, 0.0 };
}
static te_complex c_npr(te_complex n, te_complex r) {
    int ni = (int)n.real, ri = (int)r.real;
    if (ri < 0 || ri > ni || ni < 0) return (te_complex){ NAN, 0.0 };
    double res = 1.0;
    for (int i = 0; i < ri; i++) res *= (ni - i);
    return (te_complex){ res, 0.0 };
}

/* ── Funciones especiales ── */

/* erf(x): Abramowitz & Stegun 7.1.26 – error máx. 1.5e-7 */
static te_complex c_erf(te_complex z) {
    double x = z.real;
    double t = 1.0 / (1.0 + 0.3275911 * fabs(x));
    /* Horner: ((((a5*t+a4)*t+a3)*t+a2)*t+a1)*t·exp(-x²) */
    double y = 1.0 - ((((1.061405429 * t
                        - 1.453152027) * t
                        + 1.421413741) * t
                        - 0.284496736) * t
                        + 0.254829592) * t * exp(-x * x);
    return (te_complex){ (x >= 0) ? y : -y, 0.0 };
}
static te_complex c_erfc(te_complex z) {
    te_complex e = c_erf(z);
    return (te_complex){ 1.0 - e.real, 0.0 };
}

/* gamma(x): aproximación de Lanczos g=7, n=9 (error < 1e-12 para Re>0.5) */
static te_complex c_gamma(te_complex z);  /* prototipo para recursión */
static te_complex c_gamma(te_complex z) {
    double x = z.real;
    if (z.imag != 0.0) return (te_complex){ NAN, 0.0 }; /* solo real */
    if (x <= 0.0 && x == (double)(int)x) return (te_complex){ INFINITY, 0.0 }; /* polo */

    static const double g = 7.0;
    static const double p[] = {
        0.99999999999980993,
        676.5203681218851,  -1259.1392167224028,
        771.32342877765313, -176.61502916214059,
        12.507343278686905, -0.13857109526572012,
        9.9843695780195716e-6, 1.5056327351493116e-7
    };

    if (x < 0.5) {
        /* Fórmula de reflexión: Γ(x)·Γ(1-x) = π/sin(πx) */
        te_complex one_minus = { 1.0 - x, 0.0 };
        te_complex g1 = c_gamma(one_minus);
        if (!isfinite(g1.real)) return (te_complex){ NAN, 0.0 };
        double s = sin(M_PI * x);
        if (fabs(s) < 1e-300) return (te_complex){ INFINITY, 0.0 };
        return (te_complex){ M_PI / (s * g1.real), 0.0 };
    }

    x -= 1.0;
    double a = p[0];
    for (int i = 1; i <= 8; i++) a += p[i] / (x + i);
    double t = x + g + 0.5;
    double res = sqrt(2.0 * M_PI) * pow(t, x + 0.5) * exp(-t) * a;
    return (te_complex){ res, 0.0 };
}

/* ── Constantes ── */
static te_complex c_pi(void) { return (te_complex){ M_PI, 0.0 }; }
static te_complex c_e (void) { return (te_complex){ M_E,  0.0 }; }
static te_complex c_i (void) { return (te_complex){ 0.0,  1.0 }; }

/* Operador coma: descarta el primero, devuelve el segundo */
static te_complex comma(te_complex a, te_complex b) { (void)a; return b; }

/* ════════════════════════════════════════════════════════
 *  TABLA DE FUNCIONES BUILT-IN  (¡debe estar ORDENADA alfabéticamente!)
 * ════════════════════════════════════════════════════════ */
static const te_variable functions[] = {
    {"abs",   (void*)c_abs_ret, TE_FUNCTION1 | TE_FLAG_PURE, 0},
    {"acos",  (void*)c_acos,   TE_FUNCTION1 | TE_FLAG_PURE, 0},
    {"acosh", (void*)c_acosh,  TE_FUNCTION1 | TE_FLAG_PURE, 0},
    {"arg",   (void*)c_arg_ret,TE_FUNCTION1 | TE_FLAG_PURE, 0},
    {"asin",  (void*)c_asin,   TE_FUNCTION1 | TE_FLAG_PURE, 0},
    {"asinh", (void*)c_asinh,  TE_FUNCTION1 | TE_FLAG_PURE, 0},
    {"atan",  (void*)c_atan,   TE_FUNCTION1 | TE_FLAG_PURE, 0},
    {"atan2", (void*)c_atan2,  TE_FUNCTION2 | TE_FLAG_PURE, 0},
    {"atanh", (void*)c_atanh,  TE_FUNCTION1 | TE_FLAG_PURE, 0},
    {"ceil",  (void*)c_ceil,   TE_FUNCTION1 | TE_FLAG_PURE, 0},
    {"conj",  (void*)c_conj,   TE_FUNCTION1 | TE_FLAG_PURE, 0},
    {"cos",   (void*)c_cos,    TE_FUNCTION1 | TE_FLAG_PURE, 0},
    {"cosh",  (void*)c_cosh,   TE_FUNCTION1 | TE_FLAG_PURE, 0},
    {"e",     (void*)c_e,      TE_FUNCTION0 | TE_FLAG_PURE, 0},
    {"erf",   (void*)c_erf,    TE_FUNCTION1 | TE_FLAG_PURE, 0},
    {"erfc",  (void*)c_erfc,   TE_FUNCTION1 | TE_FLAG_PURE, 0},
    {"exp",   (void*)c_exp,    TE_FUNCTION1 | TE_FLAG_PURE, 0},
    {"fac",   (void*)c_fac,    TE_FUNCTION1 | TE_FLAG_PURE, 0},
    {"floor", (void*)c_floor,  TE_FUNCTION1 | TE_FLAG_PURE, 0},
    {"gamma", (void*)c_gamma,  TE_FUNCTION1 | TE_FLAG_PURE, 0},
    {"i",     (void*)c_i,      TE_FUNCTION0 | TE_FLAG_PURE, 0},
    {"im",    (void*)c_imag,   TE_FUNCTION1 | TE_FLAG_PURE, 0},
    {"imag",  (void*)c_imag,   TE_FUNCTION1 | TE_FLAG_PURE, 0},
    {"ln",    (void*)c_ln,     TE_FUNCTION1 | TE_FLAG_PURE, 0},
    {"log",   (void*)c_log10,  TE_FUNCTION1 | TE_FLAG_PURE, 0},
    {"log10", (void*)c_log10,  TE_FUNCTION1 | TE_FLAG_PURE, 0},
    {"log2",  (void*)c_log2,   TE_FUNCTION1 | TE_FLAG_PURE, 0},
    {"max",   (void*)c_max2,   TE_FUNCTION2 | TE_FLAG_PURE, 0},
    {"min",   (void*)c_min2,   TE_FUNCTION2 | TE_FLAG_PURE, 0},
    {"mod",   (void*)c_mod,    TE_FUNCTION2 | TE_FLAG_PURE, 0},
    {"ncr",   (void*)c_ncr,    TE_FUNCTION2 | TE_FLAG_PURE, 0},
    {"npr",   (void*)c_npr,    TE_FUNCTION2 | TE_FLAG_PURE, 0},
    {"pi",    (void*)c_pi,     TE_FUNCTION0 | TE_FLAG_PURE, 0},
    {"pow",   (void*)c_pow,    TE_FUNCTION2 | TE_FLAG_PURE, 0},
    {"re",    (void*)c_real,   TE_FUNCTION1 | TE_FLAG_PURE, 0},
    {"real",  (void*)c_real,   TE_FUNCTION1 | TE_FLAG_PURE, 0},
    {"round", (void*)c_round,  TE_FUNCTION1 | TE_FLAG_PURE, 0},
    {"sign",  (void*)c_sign,   TE_FUNCTION1 | TE_FLAG_PURE, 0},
    {"sin",   (void*)c_sin,    TE_FUNCTION1 | TE_FLAG_PURE, 0},
    {"sinh",  (void*)c_sinh,   TE_FUNCTION1 | TE_FLAG_PURE, 0},
    {"sqrt",  (void*)c_sqrt,   TE_FUNCTION1 | TE_FLAG_PURE, 0},
    {"tan",   (void*)c_tan,    TE_FUNCTION1 | TE_FLAG_PURE, 0},
    {"tanh",  (void*)c_tanh,   TE_FUNCTION1 | TE_FLAG_PURE, 0},
    {0, 0, 0, 0}
};

/* ────────────────────────────────────────────────────────
 *  BÚSQUEDA EN TABLA  (binaria sobre 'functions', lineal en 'lookup')
 * ──────────────────────────────────────────────────────── */
static const te_variable *find_builtin(const char *name, int len) {
    int lo = 0, hi = (int)(sizeof(functions)/sizeof(te_variable)) - 2;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int c = strncmp(name, functions[mid].name, len);
        if (!c) c = '\0' - functions[mid].name[len];
        if      (c == 0) return functions + mid;
        else if (c > 0)  lo = mid + 1;
        else             hi = mid - 1;
    }
    return 0;
}
static const te_variable *find_lookup(const state *s, const char *name, int len) {
    if (!s->lookup) return 0;
    const te_variable *v = s->lookup;
    for (int n = s->lookup_len; n; ++v, --n)
        if (strncmp(name, v->name, len) == 0 && v->name[len] == '\0') return v;
    return 0;
}

/* ────────────────────────────────────────────────────────
 *  ÁRBOL DE EXPRESIONES
 * ──────────────────────────────────────────────────────── */
static te_expr *new_expr(int type, const te_expr *params[]) {
    int arity = ARITY(type);
    int psize = sizeof(void*) * arity;
    int size  = (sizeof(te_expr) - sizeof(void*)) + psize
                + (IS_CLOSURE(type) ? sizeof(void*) : 0);
    te_expr *ret = (te_expr*)malloc(size);
    if (!ret) return NULL;
    memset(ret, 0, size);
    if (arity && params) memcpy(ret->parameters, params, psize);
    ret->type  = type;
    ret->bound = 0;
    return ret;
}

static void te_free_parameters(te_expr *n) {
    if (!n) return;
    switch (TYPE_MASK(n->type)) {
        case TE_FUNCTION7: case TE_CLOSURE7: te_free((te_expr*)n->parameters[6]); /* fall */
        case TE_FUNCTION6: case TE_CLOSURE6: te_free((te_expr*)n->parameters[5]); /* fall */
        case TE_FUNCTION5: case TE_CLOSURE5: te_free((te_expr*)n->parameters[4]); /* fall */
        case TE_FUNCTION4: case TE_CLOSURE4: te_free((te_expr*)n->parameters[3]); /* fall */
        case TE_FUNCTION3: case TE_CLOSURE3: te_free((te_expr*)n->parameters[2]); /* fall */
        case TE_FUNCTION2: case TE_CLOSURE2: te_free((te_expr*)n->parameters[1]); /* fall */
        case TE_FUNCTION1: case TE_CLOSURE1: te_free((te_expr*)n->parameters[0]);
    }
}
void te_free(te_expr *n) {
    if (!n) return;
    te_free_parameters(n);
    free(n);
}

/* ════════════════════════════════════════════════════════
 *  PARSER – declaraciones anticipadas
 * ════════════════════════════════════════════════════════ */
static void next_token(state *s);
static te_expr *list (state *s);
static te_expr *expr (state *s);
static te_expr *power(state *s);

/* ════════════════════════════════════════════════════════
 *  TOKENIZER
 * ════════════════════════════════════════════════════════ */
static void next_token(state *s) {
    s->type = TOK_NULL;
    do {
        if (!*s->next) { s->type = TOK_END; return; }

        /* Número literal: 3.14, 2i, .5 */
        if ((s->next[0] >= '0' && s->next[0] <= '9') || s->next[0] == '.') {
            s->value.real = strtod(s->next, (char**)&s->next);
            s->value.imag = 0.0;
            if (*s->next == 'i' || *s->next == 'j') {
                s->value.imag = s->value.real;
                s->value.real = 0.0;
                s->next++;
            }
            s->type = TOK_NUMBER;
        }
        /* Identificador: nombre de función o variable */
        else if (isalpha((unsigned char)s->next[0]) || s->next[0] == '_') {
            const char *start = s->next;
            while (isalnum((unsigned char)*s->next) || *s->next == '_') s->next++;
            int len = (int)(s->next - start);
            const te_variable *v = find_lookup(s, start, len);
            if (!v) v = find_builtin(start, len);
            if (!v) { s->type = TOK_ERROR; }
            else {
                switch (TYPE_MASK(v->type)) {
                    case TE_VARIABLE:
                        s->type  = TOK_VARIABLE;
                        s->bound = (const te_complex*)v->address;
                        break;
                    case TE_CLOSURE0: case TE_CLOSURE1: case TE_CLOSURE2: case TE_CLOSURE3:
                    case TE_CLOSURE4: case TE_CLOSURE5: case TE_CLOSURE6: case TE_CLOSURE7:
                        s->context = v->context; /* falls through */
                    case TE_FUNCTION0: case TE_FUNCTION1: case TE_FUNCTION2: case TE_FUNCTION3:
                    case TE_FUNCTION4: case TE_FUNCTION5: case TE_FUNCTION6: case TE_FUNCTION7:
                        s->type     = v->type;
                        s->function = v->address;
                        break;
                }
            }
        }
        /* Operadores y delimitadores */
        else {
            switch (s->next++[0]) {
                case '+': s->type = TOK_INFIX; s->function = (void*)c_add; break;
                case '-': s->type = TOK_INFIX; s->function = (void*)c_sub; break;
                case '*': s->type = TOK_INFIX; s->function = (void*)c_mul; break;
                case '/': s->type = TOK_INFIX; s->function = (void*)c_div; break;
                case '^': s->type = TOK_INFIX; s->function = (void*)c_pow; break;
                case '%': s->type = TOK_INFIX; s->function = (void*)c_mod; break;
                case '(': s->type = TOK_OPEN;  break;
                case ')': s->type = TOK_CLOSE; break;
                case ',': s->type = TOK_SEP;   break;
                case ' ': case '\t': case '\n': case '\r': break;
                default:  s->type = TOK_ERROR; break;
            }
        }
    } while (s->type == TOK_NULL);
}

/* ════════════════════════════════════════════════════════
 *  GRAMÁTICA RECURSIVA DESCENDENTE
 *  list → expr (, expr)*
 *  expr → term ((+|-) term)*
 *  term → factor ((*|/) factor)*
 *  factor → power (^ power)*
 *  power → [unary-] base
 *  base  → number | variable | function-call | (list)
 * ════════════════════════════════════════════════════════ */

static te_expr *base(state *s) {
    te_expr *ret = NULL;

    switch (TYPE_MASK(s->type)) {
        case TOK_NUMBER:
            ret = new_expr(TE_CONSTANT, 0);
            GUARD_NULL(ret);
            ret->value = s->value;
            next_token(s);
            break;

        case TOK_VARIABLE:
            ret = new_expr(TE_VARIABLE, 0);
            GUARD_NULL(ret);
            ret->bound = s->bound;
            next_token(s);
            break;

        case TE_FUNCTION0: case TE_CLOSURE0: {
            ret = new_expr(s->type, 0);
            GUARD_NULL(ret);
            ret->function = s->function;
            if (IS_CLOSURE(s->type)) ret->parameters[0] = s->context;
            next_token(s);
            if (s->type == TOK_OPEN) {
                next_token(s);
                if (s->type != TOK_CLOSE) s->type = TOK_ERROR;
                else next_token(s);
            }
            break;
        }

        case TE_FUNCTION1: case TE_CLOSURE1: {
            ret = new_expr(s->type, 0);
            GUARD_NULL(ret);
            ret->function = s->function;
            if (IS_CLOSURE(s->type)) ret->parameters[1] = s->context;
            next_token(s);
            ret->parameters[0] = power(s);
            if (!ret->parameters[0]) { te_free(ret); return NULL; }
            break;
        }

        default:
            if (TYPE_MASK(s->type) >= TE_FUNCTION2 && TYPE_MASK(s->type) <= TE_FUNCTION7) {
                int arity = ARITY(s->type);
                ret = new_expr(s->type, 0);
                GUARD_NULL(ret);
                ret->function = s->function;
                if (IS_CLOSURE(s->type)) ret->parameters[arity] = s->context;
                next_token(s);
                if (s->type != TOK_OPEN) { s->type = TOK_ERROR; te_free(ret); return NULL; }
                for (int i = 0; i < arity; i++) {
                    next_token(s);
                    ret->parameters[i] = expr(s);
                    if (!ret->parameters[i]) { te_free(ret); return NULL; }
                    if (i < arity - 1 && s->type != TOK_SEP) { s->type = TOK_ERROR; break; }
                }
                if (s->type != TOK_CLOSE) { s->type = TOK_ERROR; te_free(ret); return NULL; }
                next_token(s);
                break;
            }
            /* '(' expr ')' */
            if (s->type == TOK_OPEN) {
                next_token(s);
                ret = list(s);
                GUARD_NULL(ret);
                if (s->type != TOK_CLOSE) { s->type = TOK_ERROR; te_free(ret); return NULL; }
                next_token(s);
                break;
            }
            /* Error: token desconocido */
            ret = new_expr(TE_CONSTANT, 0);
            GUARD_NULL(ret);
            s->type     = TOK_ERROR;
            ret->value  = (te_complex){ NAN, NAN };
            break;
    }
    return ret;
}

static te_expr *power(state *s) {
    int sign = 1;
    while (s->type == TOK_INFIX &&
           (s->function == (void*)c_add || s->function == (void*)c_sub)) {
        if (s->function == (void*)c_sub) sign = -sign;
        next_token(s);
    }
    te_expr *b = base(s);
    if (!b) return NULL;
    if (sign == -1) {
        te_expr *neg = new_expr(TE_FUNCTION1 | TE_FLAG_PURE, (const te_expr*[]){b});
        if (!neg) { te_free(b); return NULL; }
        neg->function = (void*)c_neg;
        return neg;
    }
    return b;
}

static te_expr *factor(state *s) {
    te_expr *ret = power(s);
    if (!ret) return NULL;
    while (s->type == TOK_INFIX && s->function == (void*)c_pow) {
        te_fun2 t = (te_fun2)s->function;
        next_token(s);
        te_expr *p = power(s);
        if (!p) { te_free(ret); return NULL; }
        te_expr *prev = ret;
        ret = new_expr(TE_FUNCTION2 | TE_FLAG_PURE, (const te_expr*[]){prev, p});
        if (!ret) { te_free(p); te_free(prev); return NULL; }
        ret->function = (void*)t;
    }
    return ret;
}

static te_expr *term(state *s) {
    te_expr *ret = factor(s);
    if (!ret) return NULL;
    while (s->type == TOK_INFIX &&
           (s->function == (void*)c_mul || s->function == (void*)c_div ||
            s->function == (void*)c_mod)) {
        te_fun2 t = (te_fun2)s->function;
        next_token(s);
        te_expr *f = factor(s);
        if (!f) { te_free(ret); return NULL; }
        te_expr *prev = ret;
        ret = new_expr(TE_FUNCTION2 | TE_FLAG_PURE, (const te_expr*[]){prev, f});
        if (!ret) { te_free(f); te_free(prev); return NULL; }
        ret->function = (void*)t;
    }
    return ret;
}

static te_expr *expr(state *s) {
    te_expr *ret = term(s);
    if (!ret) return NULL;
    while (s->type == TOK_INFIX &&
           (s->function == (void*)c_add || s->function == (void*)c_sub)) {
        te_fun2 t = (te_fun2)s->function;
        next_token(s);
        te_expr *e = term(s);
        if (!e) { te_free(ret); return NULL; }
        te_expr *prev = ret;
        ret = new_expr(TE_FUNCTION2 | TE_FLAG_PURE, (const te_expr*[]){prev, e});
        if (!ret) { te_free(e); te_free(prev); return NULL; }
        ret->function = (void*)t;
    }
    return ret;
}

static te_expr *list(state *s) {
    te_expr *ret = expr(s);
    if (!ret) return NULL;
    while (s->type == TOK_SEP) {
        next_token(s);
        te_expr *e = expr(s);
        if (!e) { te_free(ret); return NULL; }
        te_expr *prev = ret;
        ret = new_expr(TE_FUNCTION2 | TE_FLAG_PURE, (const te_expr*[]){prev, e});
        if (!ret) { te_free(e); te_free(prev); return NULL; }
        ret->function = (void*)comma;
    }
    return ret;
}

/* ════════════════════════════════════════════════════════
 *  EVALUADOR
 * ════════════════════════════════════════════════════════ */
#define M(k) te_eval((te_expr*)n->parameters[k])

te_complex te_eval(const te_expr *n) {
    if (!n) return (te_complex){ NAN, NAN };
    switch (TYPE_MASK(n->type)) {
        case TE_CONSTANT: return n->value;
        case TE_VARIABLE: return *n->bound;
        case TE_FUNCTION0: return ((te_fun0)n->function)();
        case TE_FUNCTION1: return ((te_fun1)n->function)(M(0));
        case TE_FUNCTION2: return ((te_fun2)n->function)(M(0), M(1));
        case TE_FUNCTION3: {
            typedef te_complex (*f3)(te_complex,te_complex,te_complex);
            return ((f3)n->function)(M(0), M(1), M(2));
        }
        default: return (te_complex){ NAN, NAN };
    }
}
#undef M

/* ════════════════════════════════════════════════════════
 *  OPTIMIZADOR (plegado de constantes)
 * ════════════════════════════════════════════════════════ */
static void optimize(te_expr *n) {
    if (!n || n->type == TE_CONSTANT || n->type == TE_VARIABLE) return;
    if (IS_PURE(n->type)) {
        int arity = ARITY(n->type), known = 1;
        for (int i = 0; i < arity; i++) {
            optimize((te_expr*)n->parameters[i]);
            if (((te_expr*)n->parameters[i])->type != TE_CONSTANT) known = 0;
        }
        if (known) {
            te_complex v = te_eval(n);
            te_free_parameters(n);
            n->type  = TE_CONSTANT;
            n->value = v;
        }
    }
}

/* ════════════════════════════════════════════════════════
 *  API PÚBLICA
 * ════════════════════════════════════════════════════════ */

te_expr *te_compile(const char *expression,
                    const te_variable *variables, int var_count,
                    int *error) {
    state s;
    s.start = s.next = expression;
    s.lookup     = variables;
    s.lookup_len = var_count;
    next_token(&s);
    te_expr *root = list(&s);
    if (!root) { if (error) *error = -1; return NULL; }
    if (s.type != TOK_END) {
        te_free(root);
        if (error) { *error = (int)(s.next - s.start); if (!*error) *error = 1; }
        return NULL;
    }
    optimize(root);
    if (error) *error = 0;
    return root;
}

te_complex te_interp(const char *expression, int *error) {
    te_expr *n = te_compile(expression, 0, 0, error);
    te_complex ret = { NAN, NAN };
    if (n) { ret = te_eval(n); te_free(n); }
    return ret;
}

/* Variante con variables de usuario (para Ans, x, etc.) */
te_complex te_interp_vars(const char *expression,
                           const te_variable *vars, int var_count,
                           int *error) {
    te_expr *n = te_compile(expression, vars, var_count, error);
    te_complex ret = { NAN, NAN };
    if (n) { ret = te_eval(n); te_free(n); }
    return ret;
}

/* ════════════════════════════════════════════════════════
 *  EVALUACIÓN DE POLINOMIOS  (esquema de Horner)
 * ════════════════════════════════════════════════════════ */
static te_complex poly_eval(const double *c, int deg, te_complex x) {
    if (deg < 0) return (te_complex){ 0.0, 0.0 };
    te_complex r = { c[deg], 0.0 };
    for (int i = deg - 1; i >= 0; i--)
        r = c_add(c_mul(r, x), (te_complex){ c[i], 0.0 });
    return r;
}

static te_complex poly_eval_deriv(const double *c, int deg, te_complex x) {
    if (deg <= 0) return (te_complex){ 0.0, 0.0 };
    te_complex r = { (double)deg * c[deg], 0.0 };
    for (int i = deg - 1; i >= 1; i--)
        r = c_add(c_mul(r, x), (te_complex){ (double)i * c[i], 0.0 });
    return r;
}

/* ════════════════════════════════════════════════════════
 *  NEWTON-RAPHSON con reintentos
 *  Retorna 1 si converge, 0 si falla.
 *  El punto inicial *x se modifica; al exit contiene la raíz.
 * ════════════════════════════════════════════════════════ */
static int poly_newton(const double *c, int deg, te_complex *x) {
    const int MAX_IT       = 200;
    const int MAX_RESTARTS = 6;
    const double TOL_X     = 1e-10;
    const double TOL_F     = 1e-10;

    for (int restart = 0; restart < MAX_RESTARTS; restart++) {
        if (restart > 0) {
            /* Semilla nueva en disco de radio 2 centrado en origen */
            double angle = ((double)rand() / RAND_MAX) * 6.2831853;
            double r     = 0.5 + ((double)rand() / RAND_MAX) * 2.0;
            x->real = r * cos(angle);
            x->imag = r * sin(angle);
        }
        for (int it = 0; it < MAX_IT; it++) {
            te_complex f  = poly_eval      (c, deg, *x);
            te_complex df = poly_eval_deriv(c, deg, *x);
            double     df_abs = te_complex_abs(df);
            if (df_abs < 1e-14) break; /* derivada nula, cambiar semilla */
            te_complex step = c_div(f, df);
            *x = c_sub(*x, step);
            if (te_complex_abs(step) < TOL_X && te_complex_abs(f) < TOL_F)
                return 1;
        }
    }
    return 0;
}

/* ════════════════════════════════════════════════════════
 *  DEFLACIÓN LINEAL  (divide p(x) por (x - root) usando Horner)
 *  El residuo imaginario se descarta; es ~0 para raíces reales.
 * ════════════════════════════════════════════════════════ */
static void poly_deflate(const double *c, int deg, te_complex root, double *out) {
    if (deg <= 0) return;
    te_complex accum = { c[deg], 0.0 };
    out[deg - 1] = accum.real;
    for (int i = deg - 1; i >= 1; i--) {
        accum = c_add(c_mul(accum, root), (te_complex){ c[i], 0.0 });
        out[i - 1] = accum.real;
    }
    /* out[0..deg-2] contiene los coef. del cociente */
}

/* ════════════════════════════════════════════════════════
 *  DEFLACIÓN CUADRÁTICA  (divide p(x) por (x-r)(x-r*))
 *
 *  BUG ORIGINAL: el bucle usaba c[i+2] en lugar de c[i].
 *  Corrección: algoritmo estándar de división sintética cuadrática.
 *
 *  Divisor: x² + p·x + q,  con p = -2·Re(r),  q = |r|².
 *  Algoritmo (Horner cuadrático):
 *    b[n]   = a[n]
 *    b[n-1] = a[n-1] - p·b[n]
 *    b[k]   = a[k]   - p·b[k+1] - q·b[k+2]   (k = n-2 … 0)
 *  Cociente (grado n-2): b[n]·x^(n-2) + … + b[2]
 *  Salida (lowest-first): out[k] = b[k+2],  k = 0 … n-2
 * ════════════════════════════════════════════════════════ */
static void poly_deflate_conj(const double *c, int deg, te_complex r, double *out) {
    double p = -2.0 * r.real;          /* coef. lineal del divisor cuadrático */
    double q  = r.real*r.real + r.imag*r.imag; /* término constante            */
    double b[11] = { 0 };              /* coef. del cociente (tamaño máx. 11) */

    b[deg]     = c[deg];
    b[deg - 1] = c[deg - 1] - p * b[deg];
    for (int k = deg - 2; k >= 0; k--)
        b[k] = c[k] - p * b[k + 1] - q * b[k + 2]; /* ← c[k], no c[k+2] */

    /* Copiar cociente al buffer de salida (formato lowest-first) */
    for (int k = 0; k <= deg - 2; k++)
        out[k] = b[k + 2];
}

/* ════════════════════════════════════════════════════════
 *  SOLVER DE POLINOMIOS (grado 1–9)
 * ════════════════════════════════════════════════════════ */
int te_solve_poly(const double *coeffs, int degree,
                  te_complex *roots, int max_roots) {
    if (degree < 1 || degree > 9 || !coeffs || !roots) return 0;

    /* ── Grado 1: raíz lineal ── */
    if (degree == 1) {
        if (fabs(coeffs[1]) < 1e-14) return 0;
        roots[0] = (te_complex){ -coeffs[0] / coeffs[1], 0.0 };
        return 1;
    }

    /* ── Grado 2: fórmula cuadrática ── */
    if (degree == 2) {
        double a = coeffs[2], b = coeffs[1], c_c = coeffs[0];
        if (fabs(a) < 1e-14) return 0;
        te_complex disc = { b*b - 4.0*a*c_c, 0.0 };
        te_complex sd   = c_sqrt(disc);
        te_complex denom = { 2.0*a, 0.0 };
        roots[0] = c_div(c_add((te_complex){-b,0}, sd), denom);
        roots[1] = c_div(c_sub((te_complex){-b,0}, sd), denom);
        return 2;
    }

    /* ── Grado 3: Cardano con forma trigonométrica (ultra-estable) ── */
    if (degree == 3) {
        double a = coeffs[3], b = coeffs[2], cc = coeffs[1], d = coeffs[0];
        if (fabs(a) < 1e-14) return 0;
        double p     = (3.0*a*cc - b*b) / (3.0*a*a);
        double q     = (2.0*b*b*b - 9.0*a*b*cc + 27.0*a*a*d) / (27.0*a*a*a);
        double shift = -b / (3.0*a);
        double disc  = q*q/4.0 + p*p*p/27.0;

        if (disc > 1e-10) {
            /* 1 real + 2 complejas conjugadas */
            double sqD = sqrt(disc);
            double u   = cbrt(-q/2.0 + sqD);
            double v   = cbrt(-q/2.0 - sqD);
            roots[0] = (te_complex){ u + v + shift, 0.0 };
            double re_part = -0.5*(u+v) + shift;
            double im_part =  0.5*sqrt(3.0) * fabs(u-v);
            roots[1] = (te_complex){ re_part,  im_part };
            roots[2] = (te_complex){ re_part, -im_part };
        } else if (disc < -1e-10) {
            /* 3 raíces reales (caso irreducible) – forma trigonométrica */
            double r3 = sqrt(-p/3.0);
            if (r3 < 1e-300) { roots[0]=roots[1]=roots[2]=(te_complex){shift,0}; return 3; }
            double phi = acos(fmax(-1.0, fmin(1.0, -q/(2.0*r3*r3*r3))));
            roots[0] = (te_complex){ 2.0*r3*cos(phi/3.0)               + shift, 0.0 };
            roots[1] = (te_complex){ 2.0*r3*cos((phi + 2.0*M_PI)/3.0)  + shift, 0.0 };
            roots[2] = (te_complex){ 2.0*r3*cos((phi + 4.0*M_PI)/3.0)  + shift, 0.0 };
        } else {
            /* Raíces múltiples (disc ≈ 0) */
            double u = cbrt(-q/2.0);
            roots[0] = (te_complex){ 2.0*u + shift, 0.0 };
            roots[1] = (te_complex){  -u   + shift, 0.0 };
            roots[2] = roots[1];
        }
        return 3;
    }

    /* ── Grados 4–9: Newton + deflación ── */
    double work[10];
    memcpy(work, coeffs, sizeof(double) * (degree + 1));
    int found = 0, deg = degree;

    while (deg >= 1 && found < max_roots) {
        /* Punto inicial aleatorio */
        te_complex x0 = {
            ((double)rand()/RAND_MAX)*2.0 - 1.0,
            ((double)rand()/RAND_MAX)*2.0 - 1.0
        };

        if (!poly_newton(work, deg, &x0)) {
            /* Newton falló completamente – abandonar este grado */
            break;
        }

        /* Decidir: raíz compleja (par conjugado) o real */
        if (fabs(x0.imag) > 1e-7 && deg >= 2 && found + 1 < max_roots) {
            /* Raíz compleja: guardar z y conj(z) */
            roots[found++] = x0;
            roots[found++] = c_conj(x0);
            double next[10] = {0};
            poly_deflate_conj(work, deg, x0, next);
            memcpy(work, next, sizeof(double) * (deg - 1));
            deg -= 2;
        } else {
            /* Raíz real (o casi real) */
            x0.imag = 0.0; /* forzar real para la deflación */
            roots[found++] = x0;
            double next[10] = {0};
            poly_deflate(work, deg, x0, next);
            memcpy(work, next, sizeof(double) * deg);
            deg -= 1;
        }
    }
    return found;
}
