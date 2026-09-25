/* names.c - Mac file names: Mac Roman <-> UTF-8, validation, catalog ordering. */
#include "names.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Unicode code points of Mac Roman 0x80..0xFF (0xDB is the currency sign of
 * the systems of the 68k era; the euro sign is accepted on input as well). */
static const uint16_t mac_roman_hi[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, /* 80 */
    0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8, /* 88 */
    0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, /* 90 */
    0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC, /* 98 */
    0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF, /* A0 */
    0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8, /* A8 */
    0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211, /* B0 */
    0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8, /* B8 */
    0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, /* C0 */
    0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153, /* C8 */
    0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA, /* D0 */
    0x00FF, 0x0178, 0x2044, 0x00A4, 0x2039, 0x203A, 0xFB01, 0xFB02, /* D8 */
    0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, /* E0 */
    0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4, /* E8 */
    0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC, /* F0 */
    0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7, /* F8 */
};

static int unicode_to_mac(uint32_t cp) {
    if (cp < 0x80) return (int)cp;
    if (cp == 0x20AC) return 0xDB;
    for (int i = 0; i < 128; i++)
        if (mac_roman_hi[i] == cp) return 0x80 + i;
    return -1;
}

/* Decodes one UTF-8 sequence; returns its length or 0 if it is invalid. */
static int utf8_decode(const unsigned char *p, size_t n, uint32_t *cp) {
    if (p[0] < 0x80) { *cp = p[0]; return 1; }
    int len = p[0] >= 0xF0 ? 4 : p[0] >= 0xE0 ? 3 : p[0] >= 0xC2 ? 2 : 0;
    if (!len || (size_t)len > n || p[0] > 0xF4) return 0;
    uint32_t c = p[0] & (0x7F >> len);
    for (int i = 1; i < len; i++) {
        if ((p[i] & 0xC0) != 0x80) return 0;
        c = (c << 6) | (p[i] & 0x3F);
    }
    if ((len == 3 && c < 0x800) || (len == 4 && c < 0x10000) || c > 0x10FFFF) return 0;
    *cp = c;
    return len;
}

int name_from_utf8(const char *in, size_t n, MacName *out) {
    const unsigned char *p = (const unsigned char *)in;
    unsigned char buf[1024];
    size_t o = 0, i = 0;
    int converted = 1;
    while (i < n) {
        uint32_t cp;
        int l = utf8_decode(p + i, n - i, &cp);
        int m = l ? unicode_to_mac(cp) : -1;
        if (m < 0) { converted = 0; break; }
        if (o >= sizeof buf) return -1;
        buf[o++] = (unsigned char)m;
        i += (size_t)l;
    }
    if (!converted) {                   /* not UTF-8 (or not Mac Roman): take the bytes */
        if (n > 255) return -1;
        memcpy(out->s, p, n);
        out->len = (unsigned char)n;
        return 0;
    }
    if (o > 255) return -1;
    memcpy(out->s, buf, o);
    out->len = (unsigned char)o;
    return 0;
}

const char *name_check(const MacName *n, int max) {
    if (n->len == 0) return "empty name";
    static char msg[48];
    if (n->len > max) { snprintf(msg, sizeof msg, "longer than %d characters", max); return msg; }
    for (int i = 0; i < n->len; i++) {
        if (n->s[i] == ':') return "':' is not allowed in Mac names";
        if (n->s[i] < 0x20 || n->s[i] == 0x7F) return "control characters are not allowed";
    }
    return NULL;
}

void name_to_utf8(const unsigned char *s, int len, char *out) {
    unsigned char *o = (unsigned char *)out;
    for (int i = 0; i < len; i++) {
        unsigned c = s[i];
        if (c < 0x20 || c == 0x7F) { *o++ = '?'; continue; }
        if (c < 0x80) { *o++ = (unsigned char)c; continue; }
        unsigned u = mac_roman_hi[c - 0x80];
        if (u < 0x800) { *o++ = (unsigned char)(0xC0 | (u >> 6)); *o++ = (unsigned char)(0x80 | (u & 0x3F)); }
        else {
            *o++ = (unsigned char)(0xE0 | (u >> 12));
            *o++ = (unsigned char)(0x80 | ((u >> 6) & 0x3F));
            *o++ = (unsigned char)(0x80 | (u & 0x3F));
        }
    }
    *o = 0;
}

char *name_str(const MacName *n) {
    static char bufs[4][255 * 3 + 1];
    static int k;
    char *b = bufs[k++ & 3];
    name_to_utf8(n->s, n->len, b);
    return b;
}

void ostype_str(const unsigned char code[4], char out[16]) {
    name_to_utf8(code, 4, out);
}

int ostype_parse(const char *in, unsigned char code[4]) {
    MacName n;
    if (name_from_utf8(in, strlen(in), &n) || n.len != 4) return -1;
    memcpy(code, n.s, 4);
    return 0;
}

/* ---- catalog ordering ----
 * Each byte gets a 16-bit sort weight; names compare weight by weight, a
 * shorter name that is a prefix of a longer one comes first. ASCII letters
 * compare without case. Accented letters sort right after their base letter
 * (upper and lower case alike), in the order grave, acute, circumflex,
 * tilde, diaeresis, ring, cedilla, then slashed forms and ligatures. All
 * other characters above 0x7F sort after ASCII, by code. */
struct accent { unsigned char upper, lower, base, rank; };
static const struct accent accents[] = {
    {0xCB, 0x88, 'A', 1}, {0xE7, 0x87, 'A', 2}, {0xE5, 0x89, 'A', 3}, {0xCC, 0x8B, 'A', 4},
    {0x80, 0x8A, 'A', 5}, {0x81, 0x8C, 'A', 6}, {0xAE, 0xBE, 'A', 8},
    {0x82, 0x8D, 'C', 7},
    {0xE9, 0x8F, 'E', 1}, {0x83, 0x8E, 'E', 2}, {0xE6, 0x90, 'E', 3}, {0xE8, 0x91, 'E', 5},
    {0xED, 0x93, 'I', 1}, {0xEA, 0x92, 'I', 2}, {0xEB, 0x94, 'I', 3}, {0xEC, 0x95, 'I', 5},
    {0xF5, 0xF5, 'I', 8},
    {0x84, 0x96, 'N', 4},
    {0xF1, 0x98, 'O', 1}, {0xEE, 0x97, 'O', 2}, {0xEF, 0x99, 'O', 3}, {0xCD, 0x9B, 'O', 4},
    {0x85, 0x9A, 'O', 5}, {0xAF, 0xBF, 'O', 8}, {0xCE, 0xCF, 'O', 9},
    {0xA7, 0xA7, 'S', 8},
    {0xF4, 0x9D, 'U', 1}, {0xF2, 0x9C, 'U', 2}, {0xF3, 0x9E, 'U', 3}, {0x86, 0x9F, 'U', 5},
    {0xD9, 0xD8, 'Y', 5},
};

static uint16_t weight[256];

static void init_weights(void) {
    for (int c = 0; c < 256; c++) weight[c] = (uint16_t)(c << 8);
    for (int c = 'a'; c <= 'z'; c++) weight[c] = (uint16_t)((c - 32) << 8);
    for (size_t i = 0; i < sizeof accents / sizeof accents[0]; i++) {
        uint16_t w = (uint16_t)((accents[i].base << 8) | accents[i].rank);
        weight[accents[i].upper] = w;
        weight[accents[i].lower] = w;
    }
}

int name_cmp(const unsigned char *a, int alen, const unsigned char *b, int blen) {
    if (!weight[1]) init_weights();
    int n = alen < blen ? alen : blen;
    for (int i = 0; i < n; i++) {
        if (weight[a[i]] != weight[b[i]]) return weight[a[i]] < weight[b[i]] ? -1 : 1;
    }
    return alen - blen;
}
