/* util.h - small helpers: memory, diagnostics, big-endian bytes, files, Mac dates. */
#ifndef MAC68K_DISK_UTIL_H
#define MAC68K_DISK_UTIL_H
#include <stddef.h>
#include <stdint.h>

/* ---- memory (all of them exit on failure) ---- */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xsprintf(const char *fmt, ...);

/* ---- diagnostics ----
 * fail() prints "<what>: <message>" to stderr and exits with status 1. Every
 * command works on an in-memory copy of the image and writes it back only at
 * the very end, so exiting early never leaves a half-written image behind.
 * warn() prints "<what>: warning: <message>". */
extern const char *prog_name;
void fail(const char *what, const char *fmt, ...);
void warn(const char *what, const char *fmt, ...);

/* ---- big-endian access ---- */
unsigned get16(const unsigned char *p);
uint32_t get32(const unsigned char *p);
void put16(unsigned char *p, unsigned v);
void put32(unsigned char *p, uint32_t v);

/* ---- files ---- */
int  read_file(const char *path, unsigned char **data, size_t *len);    /* 0 = ok */
/* Writes data to a temporary file next to path and renames it over path, so
 * that path holds either the old or the new content, never a mixture. The
 * permissions of an existing file are kept. 0 = ok, errno is set otherwise. */
int  write_file_atomic(const char *path, const unsigned char *data, size_t len);
int  write_file(const char *path, const unsigned char *data, size_t len);
int  path_exists(const char *path);
const char *path_basename(const char *path);

/* ---- dates: Mac dates count seconds since 1904-01-01 00:00 local time ---- */
uint32_t mac_now(void);
void mac_date_str(uint32_t t, char out[20]);    /* "YYYY-MM-DD HH:MM", "-" for 0 */

#endif
