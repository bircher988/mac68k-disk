/* util.c - small helpers: memory, diagnostics, big-endian bytes, files, Mac dates. */
#define _POSIX_C_SOURCE 200809L
#include "util.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

const char *prog_name = "mac68k-disk";

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) fail(prog_name, "out of memory");
    return p;
}

void *xcalloc(size_t n, size_t sz) {
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) fail(prog_name, "out of memory");
    return p;
}

void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) fail(prog_name, "out of memory");
    return p;
}

char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = xmalloc(n);
    memcpy(d, s, n);
    return d;
}

char *xsprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) fail(prog_name, "out of memory");
    char *s = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return s;
}

void fail(const char *what, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fflush(stdout);
    fprintf(stderr, "%s: ", what ? what : prog_name);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

void warn(const char *what, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fflush(stdout);
    fprintf(stderr, "%s: warning: ", what ? what : prog_name);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

unsigned get16(const unsigned char *p) { return ((unsigned)p[0] << 8) | p[1]; }
uint32_t get32(const unsigned char *p) { return ((uint32_t)get16(p) << 16) | get16(p + 2); }
void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
void put32(unsigned char *p, uint32_t v) { put16(p, (unsigned)(v >> 16)); put16(p + 2, (unsigned)(v & 0xffff)); }

int read_file(const char *path, unsigned char **data, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t cap = 65536, n = 0;
    unsigned char *d = xmalloc(cap);
    for (;;) {
        if (n == cap) { cap *= 2; d = xrealloc(d, cap); }
        size_t r = fread(d + n, 1, cap - n, f);
        if (r == 0) break;
        n += r;
    }
    int err = ferror(f);
    fclose(f);
    if (err) { free(d); errno = EIO; return -1; }
    *data = d;
    *len = n;
    return 0;
}

static int write_all(int fd, const unsigned char *data, size_t len) {
    while (len) {
        ssize_t w = write(fd, data, len);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        data += w;
        len -= (size_t)w;
    }
    return 0;
}

int write_file(const char *path, const unsigned char *data, size_t len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return -1;
    if (write_all(fd, data, len) || close(fd)) { int e = errno; unlink(path); errno = e; return -1; }
    return 0;
}

int write_file_atomic(const char *path, const unsigned char *data, size_t len) {
    char *tmp = xsprintf("%s.tmpXXXXXX", path);
    int fd = mkstemp(tmp);
    if (fd < 0) { int e = errno; free(tmp); errno = e; return -1; }
    struct stat st;
    mode_t mode = 0666;
    if (stat(path, &st) == 0) mode = st.st_mode & 07777;
    else { mode_t um = umask(0); umask(um); mode &= ~um; }
    int ok = fchmod(fd, mode) == 0 && write_all(fd, data, len) == 0 && fsync(fd) == 0;
    int e = errno;
    if (close(fd) != 0 && ok) { ok = 0; e = errno; }
    if (ok && rename(tmp, path) != 0) { ok = 0; e = errno; }
    if (!ok) unlink(tmp);
    free(tmp);
    errno = e;
    return ok ? 0 : -1;
}

int path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

const char *path_basename(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* ---- dates ----
 * The classic Mac OS keeps local wall-clock time, so a Mac date is "seconds
 * since 1904-01-01 00:00" of the local calendar, with no time zone attached. */

#define MAC_TO_UNIX_DAYS 24107L        /* days from 1904-01-01 to 1970-01-01 */

/* Days since 1970-01-01 of a proleptic Gregorian date. */
static long days_from_date(long y, int m, int d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153L * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void date_from_days(long z, long *y, int *m, int *d) {
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yoe + era * 400 + (*m <= 2);
}

uint32_t mac_now(void) {
    time_t now = time(NULL);
    struct tm lt;
    if (!localtime_r(&now, &lt)) return 0;
    long days = days_from_date(lt.tm_year + 1900L, lt.tm_mon + 1, lt.tm_mday) + MAC_TO_UNIX_DAYS;
    long long secs = (long long)days * 86400 + lt.tm_hour * 3600L + lt.tm_min * 60L + lt.tm_sec;
    if (secs < 0 || secs > 0xFFFFFFFFLL) return 0;
    return (uint32_t)secs;
}

void mac_date_str(uint32_t t, char out[20]) {
    if (t == 0) { strcpy(out, "-"); return; }
    long days = (long)(t / 86400) - MAC_TO_UNIX_DAYS;
    uint32_t s = t % 86400;
    long y; int m, d;
    date_from_days(days, &y, &m, &d);
    snprintf(out, 20, "%04ld-%02d-%02d %02u:%02u", y % 10000, m, d, (unsigned)(s / 3600), (unsigned)(s / 60 % 60));
}
