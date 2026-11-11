// Tiny arithmetic expressions for the show file: numbers, named variables, + - * / % ^, parentheses,
// functions sin cos tan abs min max clamp mix pow sqrt exp log floor fract step smoothstep noise, constant pi.
#ifndef EXPR_H
#define EXPR_H

typedef struct Expr Expr;

// names[i] is variable i in ExprEval's vars; on error returns NULL and writes a message into err
Expr *ExprParse(const char *src, const char *const *names, int nNames, char *err, int errLen);
float ExprEval(const Expr *e, const float *vars);
void  ExprFree(Expr *e);

#endif
