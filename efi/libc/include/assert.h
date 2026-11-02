#ifndef _ASSERT_H
#define _ASSERT_H
void __wyse_assert(const char *expr, const char *file, int line);
#ifdef NDEBUG
#define assert(x) ((void)0)
#else
#define assert(x) ((x) ? (void)0 : __wyse_assert(#x, __FILE__, __LINE__))
#endif
#ifndef __cplusplus
#define static_assert _Static_assert
#endif
#endif
