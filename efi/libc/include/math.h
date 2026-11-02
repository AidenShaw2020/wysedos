#ifndef _MATH_H
#define _MATH_H
#define M_PI 3.14159265358979323846
#define M_LN2 0.69314718055994530942
#define M_SQRT2 1.41421356237309504880
#define HUGE_VAL __builtin_huge_val()
#define INFINITY __builtin_inff()
#define NAN __builtin_nanf("")
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)
#define isunordered(a, b) __builtin_isunordered(a, b)
double fma(double, double, double);
double fabs(double), sqrt(double), ceil(double), floor(double), trunc(double), round(double);
double nearbyint(double), rint(double), copysign(double, double), fmod(double, double);
double frexp(double, int *), ldexp(double, int), exp2(double), log2(double), pow(double, double);
double exp(double), log(double), log10(double), modf(double, double *);
double sin(double), cos(double), tan(double), atan(double), atan2(double, double);
double asin(double), acos(double), sinh(double), cosh(double), tanh(double);
long lrint(double);
long lround(double);
float fabsf(float), sqrtf(float), floorf(float), ceilf(float), sinf(float), cosf(float), tanf(float);
float powf(float, float), expf(float), logf(float), log10f(float), fmodf(float, float), roundf(float);
float exp2f(float), log2f(float), truncf(float), atanf(float), atan2f(float, float), copysignf(float, float);
long lrintf(float);
float fminf(float, float), fmaxf(float, float);
double fmin(double, double), fmax(double, double);
#endif
