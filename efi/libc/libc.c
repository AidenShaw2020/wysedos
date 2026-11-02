// Freestanding C library for the Wyse monitor: just what the PC model and the
// monitor use. Memory comes from UEFI pool (boot services stay up), output
// goes to wyse_log_write() (UDP log, optionally the console).

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>

void wyse_log_write(const char *s, size_t len);
void *wyse_pool_alloc(size_t n);
void wyse_pool_free(void *p);
uint64_t wyse_epoch_seconds(void);
void wyse_udelay(uint32_t us);
__attribute__((noreturn)) void wyse_fatal(const char *msg);

int errno;

//--------------------------------------------------------------------+
// memory and strings
//--------------------------------------------------------------------+

void *memset(void *d, int c, size_t n) {
    uint8_t *p = d;
    while (n >= 8 && !((uintptr_t)p & 7)) {
        uint64_t v = 0x0101010101010101ULL * (uint8_t)c;
        *(uint64_t *)p = v;
        p += 8;
        n -= 8;
    }
    while (n--) *p++ = (uint8_t)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n) {
    void *ret = d;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
    return ret;
}

void *memmove(void *d, const void *s, size_t n) {
    uint8_t *dp = d;
    const uint8_t *sp = s;
    if (dp == sp || !n) return d;
    if (dp < sp || dp >= sp + n) {
        while (n--) *dp++ = *sp++;
    } else {
        while (n--) dp[n] = sp[n];
    }
    return d;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    // whole words while they are equal; the byte loop finds the order
    while (n >= 8) {
        uint64_t u, v;
        __builtin_memcpy(&u, x, 8);
        __builtin_memcpy(&v, y, 8);
        if (u != v) break;
        x += 8;
        y += 8;
        n -= 8;
    }
    for (; n; n--, x++, y++)
        if (*x != *y) return *x - *y;
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const uint8_t *p = s;
    for (; n; n--, p++)
        if (*p == (uint8_t)c) return (void *)p;
    return NULL;
}

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
size_t strnlen(const char *s, size_t m) { size_t n = 0; while (n < m && s[n]) n++; return n; }
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)); return r; }
char *strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
char *strncat(char *d, const char *s, size_t n) {
    char *p = d + strlen(d);
    while (n-- && *s) *p++ = *s++;
    *p = 0;
    return d;
}
size_t strlcpy(char *d, const char *s, size_t n) {
    size_t l = strlen(s);
    if (n) {
        size_t c = l < n - 1 ? l : n - 1;
        memcpy(d, s, c);
        d[c] = 0;
    }
    return l;
}
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return (uint8_t)*a - (uint8_t)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        if (*a != *b) return (uint8_t)*a - (uint8_t)*b;
        if (!*a) return 0;
    }
    return 0;
}
int strcasecmp(const char *a, const char *b) {
    while (*a && tolower((uint8_t)*a) == tolower((uint8_t)*b)) a++, b++;
    return tolower((uint8_t)*a) - tolower((uint8_t)*b);
}
int strncasecmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        int x = tolower((uint8_t)*a), y = tolower((uint8_t)*b);
        if (x != y) return x - y;
        if (!x) return 0;
    }
    return 0;
}
char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return NULL;
    }
}
char *strrchr(const char *s, int c) {
    const char *r = NULL;
    for (;; s++) {
        if (*s == (char)c) r = s;
        if (!*s) return (char *)r;
    }
}
char *strstr(const char *h, const char *n) {
    size_t l = strlen(n);
    if (!l) return (char *)h;
    for (; *h; h++)
        if (!strncmp(h, n, l)) return (char *)h;
    return NULL;
}
size_t strspn(const char *s, const char *a) { size_t n = 0; while (s[n] && strchr(a, s[n])) n++; return n; }
size_t strcspn(const char *s, const char *r) { size_t n = 0; while (s[n] && !strchr(r, s[n])) n++; return n; }
char *strpbrk(const char *s, const char *a) { s += strcspn(s, a); return *s ? (char *)s : NULL; }
char *strtok(char *s, const char *delim) {
    static char *save;
    if (!s) s = save;
    if (!s) return NULL;
    s += strspn(s, delim);
    if (!*s) { save = NULL; return NULL; }
    char *e = s + strcspn(s, delim);
    if (*e) { *e = 0; save = e + 1; } else save = NULL;
    return s;
}
char *strdup(const char *s) {
    size_t l = strlen(s) + 1;
    char *d = malloc(l);
    if (d) memcpy(d, s, l);
    return d;
}
const char *strerror(int e) { (void)e; return "error"; }

//--------------------------------------------------------------------+
// heap: UEFI pool with a size header (16 bytes keeps alignment)
//--------------------------------------------------------------------+

void *malloc(size_t n) {
    // zeroed: the PC model was written against an allocator that handed out
    // clean memory and does not always initialise every field itself
    uint64_t *p = wyse_pool_alloc(n + 16);
    if (!p) return NULL;
    memset(p + 2, 0, n);
    p[0] = n;
    return p + 2;
}
void free(void *p) { if (p) wyse_pool_free((uint64_t *)p - 2); }
void *calloc(size_t n, size_t m) {
    size_t t = n * m;
    void *p = malloc(t);
    if (p) memset(p, 0, t);
    return p;
}
void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    if (!n) { free(p); return NULL; }
    size_t old = ((uint64_t *)p)[-2];
    void *q = malloc(n);
    if (!q) return NULL;
    memcpy(q, p, old < n ? old : n);
    free(p);
    return q;
}
void *aligned_alloc(size_t align, size_t n) {
    // pool memory is 8-aligned; over-allocate and keep the raw pointer behind the block
    uint8_t *raw = malloc(n + align + 16);
    if (!raw) return NULL;
    uintptr_t a = ((uintptr_t)raw + 16 + align - 1) & ~(uintptr_t)(align - 1);
    ((void **)a)[-1] = raw;
    return (void *)a;
}

//--------------------------------------------------------------------+
// numbers
//--------------------------------------------------------------------+

int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }
long long llabs(long long x) { return x < 0 ? -x : x; }

unsigned long long strtoull(const char *s, char **end, int base) {
    while (isspace((uint8_t)*s)) s++;
    bool neg = false;
    if (*s == '+' || *s == '-') neg = *s++ == '-';
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && isxdigit((uint8_t)s[2])) {
        s += 2;
        base = 16;
    } else if (base == 0) {
        base = s[0] == '0' ? 8 : 10;
    }
    unsigned long long v = 0;
    const char *start = s;
    for (;; s++) {
        int d;
        if (isdigit((uint8_t)*s)) d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
    }
    if (end) *end = (char *)(s == start ? start : s);
    return neg ? -v : v;
}
long long strtoll(const char *s, char **end, int base) { return (long long)strtoull(s, end, base); }
long strtol(const char *s, char **end, int base) { return (long)strtoll(s, end, base); }
unsigned long strtoul(const char *s, char **end, int base) { return (unsigned long)strtoull(s, end, base); }
int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }

double strtod(const char *s, char **end) {
    while (isspace((uint8_t)*s)) s++;
    double sign = 1, v = 0;
    if (*s == '+' || *s == '-') sign = *s++ == '-' ? -1 : 1;
    while (isdigit((uint8_t)*s)) v = v * 10 + (*s++ - '0');
    if (*s == '.') {
        double f = 0.1;
        for (s++; isdigit((uint8_t)*s); s++, f /= 10) v += (*s - '0') * f;
    }
    if (*s == 'e' || *s == 'E') {
        char *e;
        long ex = strtol(s + 1, &e, 10);
        if (e != s + 1) {
            s = e;
            while (ex > 0) { v *= 10; ex--; }
            while (ex < 0) { v /= 10; ex++; }
        }
    }
    if (end) *end = (char *)s;
    return sign * v;
}
float strtof(const char *s, char **end) { return (float)strtod(s, end); }
double atof(const char *s) { return strtod(s, NULL); }

static uint32_t rand_state = 1;
int rand(void) { rand_state = rand_state * 1103515245 + 12345; return (rand_state >> 1) & RAND_MAX; }
void srand(unsigned seed) { rand_state = seed; }

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    // insertion sort: the model sorts short lists only
    uint8_t *b = base, tmp[256];
    if (size > sizeof(tmp)) return;
    for (size_t i = 1; i < n; i++) {
        memcpy(tmp, b + i * size, size);
        size_t j = i;
        while (j > 0 && cmp(b + (j - 1) * size, tmp) > 0) {
            memcpy(b + j * size, b + (j - 1) * size, size);
            j--;
        }
        memcpy(b + j * size, tmp, size);
    }
}

void *bsearch(const void *key, const void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    const uint8_t *b = base;
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = cmp(key, b + mid * size);
        if (!c) return (void *)(b + mid * size);
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}

void abort(void) { wyse_fatal("abort()"); }
void exit(int code) { (void)code; wyse_fatal("exit()"); }
char *getenv(const char *name) { (void)name; return NULL; }

void __wyse_assert(const char *expr, const char *file, int line) {
    printf("assert failed: %s at %s:%d\n", expr, file, line);
}

//--------------------------------------------------------------------+
// printf family
//--------------------------------------------------------------------+

typedef struct {
    char *buf;
    size_t size, len;
} out_t;

static void put(out_t *o, char c) {
    if (o->len + 1 < o->size) o->buf[o->len] = c;
    o->len++;
}

static void pad(out_t *o, char c, int n) { while (n-- > 0) put(o, c); }

static void put_str(out_t *o, const char *s, int len, int width, bool left) {
    if (!left) pad(o, ' ', width - len);
    for (int i = 0; i < len; i++) put(o, s[i]);
    if (left) pad(o, ' ', width - len);
}

static void put_int(out_t *o, unsigned long long v, bool neg, int base, bool upper, int width, int prec,
                    bool left, bool zero, char sign, bool alt) {
    char tmp[72];
    int n = 0;
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v || prec != 0) {
        do { tmp[n++] = dig[v % base]; v /= base; } while (v);
    }
    int digits = n < prec ? prec : n;
    char pre[3];
    int pl = 0;
    if (neg) pre[pl++] = '-';
    else if (sign) pre[pl++] = sign;
    if (alt && base == 16 && n) { pre[pl++] = '0'; pre[pl++] = upper ? 'X' : 'x'; }
    if (alt && base == 8 && (n == 0 || tmp[n - 1] != '0')) digits = digits > n ? digits : n + 1;
    int total = pl + digits;
    if (zero && !left && prec < 0) { for (int i = 0; i < pl; i++) put(o, pre[i]); pad(o, '0', width - total); }
    else { if (!left) pad(o, ' ', width - total); for (int i = 0; i < pl; i++) put(o, pre[i]); }
    pad(o, '0', digits - n);
    while (n) put(o, tmp[--n]);
    if (left) pad(o, ' ', width - total);
}

static void put_float(out_t *o, double v, int width, int prec, bool left, bool zero, char sign) {
    char tmp[64];
    int n = 0;
    if (prec < 0) prec = 6;
    if (prec > 15) prec = 15;
    bool neg = v < 0;
    if (neg) v = -v;
    if (v != v) { put_str(o, "nan", 3, width, left); return; }
    if (v > 1e18) { put_str(o, neg ? "-inf" : "inf", neg ? 4 : 3, width, left); return; }
    double r = 0.5;
    for (int i = 0; i < prec; i++) r /= 10;
    v += r;
    unsigned long long ip = (unsigned long long)v;
    double fp = v - (double)ip;
    char ib[24];
    int il = 0;
    do { ib[il++] = '0' + ip % 10; ip /= 10; } while (ip);
    if (neg) tmp[n++] = '-';
    else if (sign) tmp[n++] = sign;
    while (il) tmp[n++] = ib[--il];
    if (prec) {
        tmp[n++] = '.';
        for (int i = 0; i < prec; i++) {
            fp *= 10;
            int d = (int)fp;
            tmp[n++] = '0' + d;
            fp -= d;
        }
    }
    if (zero && !left && n < width) {
        int lead = (tmp[0] == '-' || tmp[0] == '+') ? 1 : 0;
        for (int i = 0; i < lead; i++) put(o, tmp[i]);
        pad(o, '0', width - n);
        for (int i = lead; i < n; i++) put(o, tmp[i]);
        return;
    }
    put_str(o, tmp, n, width, left);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    out_t o = {buf, size, 0};
    for (; *fmt; fmt++) {
        if (*fmt != '%') { put(&o, *fmt); continue; }
        fmt++;
        bool left = false, zero = false, alt = false;
        char sign = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = true;
            else if (*fmt == '0') zero = true;
            else if (*fmt == '+') sign = '+';
            else if (*fmt == ' ') { if (!sign) sign = ' '; }
            else if (*fmt == '#') alt = true;
            else break;
        }
        int width = 0;
        if (*fmt == '*') { width = va_arg(ap, int); if (width < 0) { left = true; width = -width; } fmt++; }
        else while (isdigit((uint8_t)*fmt)) width = width * 10 + (*fmt++ - '0');
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (isdigit((uint8_t)*fmt)) prec = prec * 10 + (*fmt++ - '0');
        }
        int lng = 0;     // -2 hh, -1 h, 1 l, 2 ll/j/z/t
        for (;; fmt++) {
            if (*fmt == 'h') lng--;
            else if (*fmt == 'l') lng++;
            else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't' || *fmt == 'L') lng = 2;
            else break;
        }
        switch (*fmt) {
        case 'c': { char c = (char)va_arg(ap, int); put_str(&o, &c, 1, width, left); break; }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = (int)(prec >= 0 ? strnlen(s, prec) : strlen(s));
            put_str(&o, s, len, width, left);
            break;
        }
        case 'd': case 'i': {
            long long v = lng >= 2 ? va_arg(ap, long long) : lng == 1 ? va_arg(ap, long) : va_arg(ap, int);
            if (lng == -1) v = (short)v;
            if (lng <= -2) v = (signed char)v;
            put_int(&o, v < 0 ? -(unsigned long long)v : (unsigned long long)v, v < 0, 10, false, width, prec, left, zero, sign, false);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            unsigned long long v = lng >= 2 ? va_arg(ap, unsigned long long) : lng == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
            if (lng == -1) v = (unsigned short)v;
            if (lng <= -2) v = (unsigned char)v;
            int base = *fmt == 'u' ? 10 : *fmt == 'o' ? 8 : 16;
            put_int(&o, v, false, base, *fmt == 'X', width, prec, left, zero, 0, alt);
            break;
        }
        case 'p':
            put_int(&o, (uintptr_t)va_arg(ap, void *), false, 16, false, width, prec, left, zero, 0, true);
            break;
        case 'f': case 'F': case 'g': case 'G': case 'e': case 'E':
            put_float(&o, va_arg(ap, double), width, prec, left, zero, sign);
            break;
        case 'n': *va_arg(ap, int *) = (int)o.len; break;
        case '%': put(&o, '%'); break;
        case 0: fmt--; break;
        default: put(&o, '%'); put(&o, *fmt); break;
        }
    }
    if (size) buf[o.len < size ? o.len : size - 1] = 0;
    return (int)o.len;
}

int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}
int vsprintf(char *buf, const char *fmt, va_list ap) { return vsnprintf(buf, 4096, fmt, ap); }
int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, 4096, fmt, ap);
    va_end(ap);
    return r;
}

struct _wyse_file { int fd; };
static struct _wyse_file f_in = {0}, f_out = {1}, f_err = {2};
FILE *stdin = &f_in, *stdout = &f_out, *stderr = &f_err;

int vprintf(const char *fmt, va_list ap) {
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    wyse_log_write(buf, n < (int)sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
    return n;
}
int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}
int vfprintf(FILE *f, const char *fmt, va_list ap) { (void)f; return vprintf(fmt, ap); }
int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}
int puts(const char *s) { wyse_log_write(s, strlen(s)); wyse_log_write("\n", 1); return 0; }
int fputs(const char *s, FILE *f) { (void)f; wyse_log_write(s, strlen(s)); return 0; }
int putchar(int c) { char ch = (char)c; wyse_log_write(&ch, 1); return c; }
int fputc(int c, FILE *f) { (void)f; return putchar(c); }
int fflush(FILE *f) { (void)f; return 0; }
void perror(const char *s) { printf("%s: error\n", s ? s : ""); }

// stdio files are not used by the model on this target (FatFs-style calls are)
FILE *fopen(const char *path, const char *mode) { (void)path; (void)mode; return NULL; }
int fclose(FILE *f) { (void)f; return 0; }
size_t fread(void *p, size_t s, size_t n, FILE *f) { (void)p; (void)s; (void)n; (void)f; return 0; }
size_t fwrite(const void *p, size_t s, size_t n, FILE *f) { (void)p; (void)s; (void)f; return n; }
int fseek(FILE *f, long off, int whence) { (void)f; (void)off; (void)whence; return -1; }
long ftell(FILE *f) { (void)f; return -1; }
char *fgets(char *s, int n, FILE *f) { (void)s; (void)n; (void)f; return NULL; }
int getchar(void) { return EOF; }

int sscanf(const char *s, const char *fmt, ...) {
    // integers (%d %u %x %i), strings (%s) and literals; enough for config parsing
    va_list ap;
    va_start(ap, fmt);
    int count = 0;
    while (*fmt && *s) {
        if (isspace((uint8_t)*fmt)) { while (isspace((uint8_t)*s)) s++; fmt++; continue; }
        if (*fmt != '%') { if (*fmt++ != *s++) break; continue; }
        fmt++;
        int lng = 0;
        while (*fmt == 'l' || *fmt == 'h') { if (*fmt == 'l') lng++; fmt++; }
        char *end;
        if (*fmt == 'd' || *fmt == 'i' || *fmt == 'u' || *fmt == 'x') {
            int base = *fmt == 'x' ? 16 : *fmt == 'i' ? 0 : 10;
            long long v = strtoll(s, &end, base);
            if (end == s) break;
            if (lng >= 2) *va_arg(ap, long long *) = v;
            else if (lng == 1) *va_arg(ap, long *) = (long)v;
            else *va_arg(ap, int *) = (int)v;
            s = end;
        } else if (*fmt == 's') {
            char *d = va_arg(ap, char *);
            while (isspace((uint8_t)*s)) s++;
            while (*s && !isspace((uint8_t)*s)) *d++ = *s++;
            *d = 0;
        } else {
            break;
        }
        count++;
        fmt++;
    }
    va_end(ap);
    return count;
}

//--------------------------------------------------------------------+
// time
//--------------------------------------------------------------------+

time_t time(time_t *t) {
    time_t v = (time_t)wyse_epoch_seconds();
    if (t) *t = v;
    return v;
}

clock_t clock(void) { return 0; }

struct tm *gmtime_r(const time_t *t, struct tm *r) {
    long long s = *t;
    long long days = s / 86400;
    long rem = (long)(s % 86400);
    r->tm_hour = rem / 3600;
    r->tm_min = rem / 60 % 60;
    r->tm_sec = rem % 60;
    r->tm_wday = (int)((days + 4) % 7);
    // civil-from-days (Howard Hinnant)
    long long z = days + 719468, era = z / 146097;
    long doe = (long)(z - era * 146097);
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = yoe + era * 400;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    long d = doy - (153 * mp + 2) / 5 + 1;
    long m = mp + (mp < 10 ? 3 : -9);
    r->tm_year = (int)(y + (m <= 2) - 1900);
    r->tm_mon = (int)m - 1;
    r->tm_mday = (int)d;
    r->tm_yday = 0;
    r->tm_isdst = 0;
    return r;
}
struct tm *localtime_r(const time_t *t, struct tm *r) { return gmtime_r(t, r); }
struct tm *gmtime(const time_t *t) { static struct tm r; return gmtime_r(t, &r); }
struct tm *localtime(const time_t *t) { static struct tm r; return gmtime_r(t, &r); }
time_t mktime(struct tm *tm) {
    long long y = tm->tm_year + 1900, m = tm->tm_mon + 1;
    y -= m <= 2;
    long long era = y / 400;
    long yoe = (long)(y - era * 400);
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + tm->tm_mday - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + doe - 719468;
    return days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
}

int usleep(unsigned us) { wyse_udelay(us); return 0; }
unsigned sleep(unsigned s) { wyse_udelay(s * 1000000u); return 0; }
