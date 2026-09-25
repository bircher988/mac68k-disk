/* names.h - Mac file names: Mac Roman <-> UTF-8, validation, catalog ordering. */
#ifndef MAC68K_DISK_NAMES_H
#define MAC68K_DISK_NAMES_H
#include <stddef.h>

/* A Mac name: up to 255 bytes of Mac Roman. */
typedef struct { unsigned char len; unsigned char s[255]; } MacName;

/* Converts a command-line string to Mac Roman. Valid UTF-8 whose characters
 * all exist in Mac Roman is converted; anything else is taken byte for byte.
 * Returns 0, or -1 if the result would be longer than 255 bytes. */
int name_from_utf8(const char *in, size_t n, MacName *out);

/* Checks a file, folder or volume name: 1..max bytes, no ':' and no control
 * characters. Returns NULL if it is fine, otherwise a short reason. */
const char *name_check(const MacName *n, int max);

/* Mac Roman -> UTF-8 (NUL-terminated, at most 3 bytes per character). */
void name_to_utf8(const unsigned char *s, int len, char *out);
char *name_str(const MacName *n);        /* same, into a static rotating buffer */

/* Four-character codes (file type, creator): printable form and parsing. */
void ostype_str(const unsigned char code[4], char out[16]);
int  ostype_parse(const char *in, unsigned char code[4]);      /* 0 = ok */

/* Order of names in the HFS catalog: case-insensitive, accented letters right
 * after their base letter. Returns <0, 0 or >0. Two names that compare equal
 * cannot live in the same folder. */
int name_cmp(const unsigned char *a, int alen, const unsigned char *b, int blen);

#endif
