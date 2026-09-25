/* macbin.c - MacBinary I/II/III reader and MacBinary II writer.
 *
 * A MacBinary file is a 128-byte header followed by the data fork and the
 * resource fork, each padded to a multiple of 128 bytes. Header fields used
 * here (offsets in bytes):
 *   0 version (0)   1 name length   2..64 name   65 type   69 creator
 *   73 Finder flags, high byte   75 vertical, 77 horizontal icon position
 *   79 window/folder   81 protected   82 zero   83 data fork length
 *   87 resource fork length   91 creation date   95 modification date
 *   99 comment length (II)   101 Finder flags, low byte (II)
 *   102 "mBIN" (III)   120 secondary header length (II)
 *   122 writer version (129 = II, 130 = III)   123 minimum reader version
 *   124 CRC-16/XMODEM of bytes 0..123 (II) */
#include "macbin.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>

uint16_t macbin_crc(const unsigned char *p, size_t n) {
    uint16_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)(p[i] << 8);
        for (int b = 0; b < 8; b++) crc = (uint16_t)((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}

static uint64_t pad128(uint64_t n) { return (n + 127) & ~(uint64_t)127; }

int macbin_detect(const unsigned char *b, size_t len) {
    if (len < 128 || b[0] != 0 || b[74] != 0 || b[82] != 0) return 0;
    if (b[1] < 1 || b[1] > 63) return 0;
    uint64_t dlen = get32(b + 83), rlen = get32(b + 87);
    if (dlen > 0x7FFFFFFF || rlen > 0x7FFFFFFF) return 0;
    if (get16(b + 124) == macbin_crc(b, 124)) {
        uint64_t sec = get16(b + 120);
        uint64_t need = 128 + pad128(sec) + (rlen ? pad128(dlen) + rlen : dlen);
        if (len < need) return 0;
        return memcmp(b + 102, "mBIN", 4) == 0 ? 3 : 2;
    }
    for (int i = 99; i < 128; i++) if (b[i]) return 0;
    uint64_t exact = 128 + pad128(dlen) + pad128(rlen);
    uint64_t tight = 128 + (rlen ? pad128(dlen) + rlen : dlen);
    return (len == exact || len == tight) ? 1 : 0;
}

int macbin_parse(const unsigned char *b, size_t len, MacFile *f) {
    int ver = macbin_detect(b, len);
    if (!ver) return -1;
    memset(f, 0, sizeof *f);
    f->name.len = b[1];
    memcpy(f->name.s, b + 2, b[1]);
    memcpy(f->type, b + 65, 4);
    memcpy(f->creator, b + 69, 4);
    f->flags = ((unsigned)b[73] << 8) | (ver >= 2 ? b[101] : 0);
    f->v = (int16_t)get16(b + 75);
    f->h = (int16_t)get16(b + 77);
    f->fldr = (int16_t)get16(b + 79);
    f->crdate = get32(b + 91);
    f->mddate = get32(b + 95);
    uint64_t off = 128 + (ver >= 2 ? pad128(get16(b + 120)) : 0);
    f->dlen = get32(b + 83);
    f->rlen = get32(b + 87);
    f->data = b + off;
    uint64_t roff = off + pad128(f->dlen);
    if (off + f->dlen > len || (f->rlen && roff + f->rlen > len)) return -1;
    f->rsrc = b + (roff <= len ? roff : len);
    return 0;
}

unsigned char *macbin_build(const MacFile *f, size_t *outlen) {
    size_t total = 128 + (size_t)pad128(f->dlen) + (size_t)pad128(f->rlen);
    unsigned char *o = xcalloc(total, 1);
    int nl = f->name.len > 63 ? 63 : f->name.len;
    o[1] = (unsigned char)nl;
    memcpy(o + 2, f->name.s, (size_t)nl);
    memcpy(o + 65, f->type, 4);
    memcpy(o + 69, f->creator, 4);
    o[73] = (unsigned char)(f->flags >> 8);
    put16(o + 75, (unsigned)f->v & 0xFFFF);
    put16(o + 77, (unsigned)f->h & 0xFFFF);
    put16(o + 79, (unsigned)f->fldr & 0xFFFF);
    put32(o + 83, f->dlen);
    put32(o + 87, f->rlen);
    put32(o + 91, f->crdate);
    put32(o + 95, f->mddate);
    o[101] = (unsigned char)(f->flags & 0xFF);
    o[122] = 129;
    o[123] = 129;
    put16(o + 124, macbin_crc(o, 124));
    if (f->dlen) memcpy(o + 128, f->data, f->dlen);
    if (f->rlen) memcpy(o + 128 + pad128(f->dlen), f->rsrc, f->rlen);
    *outlen = total;
    return o;
}
