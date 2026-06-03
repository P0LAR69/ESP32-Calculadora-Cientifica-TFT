// SPDX-License-Identifier: Zlib
/*
 * TINYEXPR COMPLEX MEJORADA v2.0
 * Basado en TinyExpr por Lewis Van Winkle.
 * Correcciones y mejoras: deflación cuadrática, Newton robusto,
 * nuevas funciones (asinh, acosh, erf, gamma, log2, mod, sign, round, min, max).
 */

#ifndef TINYEXPR_COMPLEX_MEJ_H
#define TINYEXPR_COMPLEX_MEJ_H

#ifdef __cplusplus
extern "C" {
#endif

/* ── Número complejo ── */
typedef struct te_complex {
    double real;
    double imag;
} te_complex;

/* ── Nodo de expresión compilada ── */
typedef struct te_expr {
    int type;
    union {
        te_complex      value;
        const te_complex *bound;
        const void      *function;
    };
    void *parameters[1];
} te_expr;

/* ── Tipos de nodo ── */
enum {
    TE_VARIABLE  = 0,
    TE_FUNCTION0 = 8,  TE_FUNCTION1, TE_FUNCTION2, TE_FUNCTION3,
    TE_FUNCTION4, TE_FUNCTION5, TE_FUNCTION6, TE_FUNCTION7,
    TE_CLOSURE0  = 16, TE_CLOSURE1,  TE_CLOSURE2,  TE_CLOSURE3,
    TE_CLOSURE4,  TE_CLOSURE5,  TE_CLOSURE6,  TE_CLOSURE7,
    TE_FLAG_PURE = 32
};

/* ── Variable de usuario para te_compile ── */
typedef struct te_variable {
    const char *name;
    const void *address;
    int         type;
    void       *context;
} te_variable;

/* ── API principal ── */
te_complex  te_interp   (const char *expression, int *error);
te_complex  te_interp_vars(const char *expression,
                            const te_variable *vars, int var_count,
                            int *error);
te_expr    *te_compile  (const char *expression,
                          const te_variable *variables, int var_count,
                          int *error);
te_complex  te_eval     (const te_expr *n);
void        te_free     (te_expr *n);

/* ── Utilidades complejas ── */
te_complex  te_complex_make(double real, double imag);
double      te_complex_abs (te_complex z);
double      te_complex_arg (te_complex z);

/* ── Solver de polinomios (grado 1–9) ──
 *  coeffs[0] = constante, coeffs[degree] = coef. líder.
 *  Devuelve número de raíces encontradas (≤ degree).
 */
int te_solve_poly(const double *coeffs, int degree,
                  te_complex *roots,    int max_roots);

#ifdef __cplusplus
}
#endif

#endif /* TINYEXPR_COMPLEX_MEJ_H */
