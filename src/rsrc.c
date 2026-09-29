/* rsrc.c - reading and writing resource forks.
 *
 * Layout (Inside Macintosh I-128, big-endian):
 *   header   0 data offset, 4 map offset, 8 data length, 12 map length;
 *            the rest of the first 256 bytes is unused
 *   data     each resource: 4-byte length, then the bytes
 *   map      16 bytes (a copy of the header), 4 next map handle, 2 file
 *            reference, 2 attributes, 2 offset of the type list, 2 offset of
 *            the name list (both from the start of the map)
 *   types    2 number of types - 1, then per type: 4 type, 2 number of
 *            resources - 1, 2 offset of its reference list (from the start
 *            of the type list)
 *   refs     12 bytes each: 2 ID, 2 name offset (-1 = none, from the start
 *            of the name list), 1 attributes, 3 data offset (from the start
 *            of the data), 4 handle
 *   names    Pascal strings */
#include "rsrc.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>

int rsrc_parse(const unsigned char *b, uint32_t len, ResFile *rf) {
    memset(rf, 0, sizeof *rf);
    if (len == 0) return 0;
    if (len < 16) return -1;
    uint32_t doff = get32(b), moff = get32(b + 4), dlen = get32(b + 8), mlen = get32(b + 12);
    if (doff > len || dlen > len - doff || moff > len || mlen > len - moff || mlen < 30) return -1;
    const unsigned char *m = b + moff;
    rf->attrs = get16(m + 22);
    unsigned tl = get16(m + 24), nl = get16(m + 26);
    if (tl + 2 > mlen) return -1;
    int ntypes = (int16_t)get16(m + tl) + 1;
    if (ntypes < 0 || tl + 2 + (size_t)ntypes * 8 > mlen) return -1;
    for (int t = 0; t < ntypes; t++) {
        const unsigned char *te = m + tl + 2 + t * 8;
        int count = (int16_t)get16(te + 4) + 1;
        unsigned refs = tl + get16(te + 6);
        if (count < 0 || refs + (size_t)count * 12 > mlen) { rsrc_free(rf); return -1; }
        for (int k = 0; k < count; k++) {
            const unsigned char *r = m + refs + k * 12;
            uint32_t off = ((uint32_t)r[5] << 16) | ((uint32_t)r[6] << 8) | r[7];
            if (off + 4 > dlen || get32(b + doff + off) > dlen - off - 4) { rsrc_free(rf); return -1; }
            Res *x = rsrc_put(rf, "", 0, NULL, b + doff + off + 4, get32(b + doff + off));
            memcpy(x->type, te, 4);
            x->id = (int16_t)get16(r);
            x->attrs = r[4];
            int noff = (int16_t)get16(r + 2);
            if (noff >= 0) {
                if (nl + (size_t)noff >= mlen || nl + (size_t)noff + 1 + m[nl + noff] > mlen) { rsrc_free(rf); return -1; }
                x->has_name = 1;
                x->name.len = m[nl + noff];
                memcpy(x->name.s, m + nl + noff + 1, x->name.len);
            }
        }
    }
    return 0;
}

unsigned char *rsrc_build(const ResFile *rf, uint32_t *len) {
    /* distinct types in order of appearance */
    int *type_of = xcalloc((size_t)rf->n + 1, sizeof *type_of), ntypes = 0;
    const unsigned char **types = xcalloc((size_t)rf->n + 1, sizeof *types);
    for (int i = 0; i < rf->n; i++) {
        int t = 0;
        while (t < ntypes && memcmp(types[t], rf->v[i].type, 4)) t++;
        if (t == ntypes) types[ntypes++] = rf->v[i].type;
        type_of[i] = t;
    }
    uint32_t dlen = 0, nlen = 0;
    for (int i = 0; i < rf->n; i++) {
        dlen += 4 + rf->v[i].len;
        if (rf->v[i].has_name) nlen += 1 + rf->v[i].name.len;
    }
    uint32_t tl_len = 2 + (uint32_t)ntypes * 8 + (uint32_t)rf->n * 12;
    uint32_t mlen = 28 + tl_len + nlen, doff = 256, moff = doff + dlen;
    *len = moff + mlen;
    unsigned char *b = xcalloc(*len, 1), *m = b + moff;
    put32(b, doff); put32(b + 4, moff); put32(b + 8, dlen); put32(b + 12, mlen);
    memcpy(m, b, 16);
    put16(m + 22, rf->attrs);
    put16(m + 24, 28);
    put16(m + 26, 28 + tl_len);
    unsigned char *tl = m + 28;
    put16(tl, (unsigned)(ntypes - 1) & 0xFFFF);
    uint32_t refpos = 2 + (uint32_t)ntypes * 8, dpos = 0, npos = 0;
    for (int t = 0; t < ntypes; t++) {
        int count = 0;
        for (int i = 0; i < rf->n; i++) if (type_of[i] == t) count++;
        unsigned char *te = tl + 2 + t * 8;
        memcpy(te, types[t], 4);
        put16(te + 4, (unsigned)(count - 1));
        put16(te + 6, refpos);
        for (int i = 0; i < rf->n; i++) {
            if (type_of[i] != t) continue;
            const Res *x = &rf->v[i];
            unsigned char *r = tl + refpos;
            put16(r, (unsigned)x->id & 0xFFFF);
            if (x->has_name) {
                put16(r + 2, npos);
                m[28 + tl_len + npos] = x->name.len;
                memcpy(m + 28 + tl_len + npos + 1, x->name.s, x->name.len);
                npos += 1 + x->name.len;
            } else put16(r + 2, 0xFFFF);
            r[4] = (unsigned char)x->attrs;
            r[5] = (unsigned char)(dpos >> 16); r[6] = (unsigned char)(dpos >> 8); r[7] = (unsigned char)dpos;
            put32(b + doff + dpos, x->len);
            if (x->len) memcpy(b + doff + dpos + 4, x->data, x->len);
            dpos += 4 + x->len;
            refpos += 12;
        }
    }
    free(type_of);
    free(types);
    return b;
}

Res *rsrc_get(ResFile *rf, const char *type, int id) {
    for (int i = 0; i < rf->n; i++)
        if (!memcmp(rf->v[i].type, type, 4) && rf->v[i].id == id) return &rf->v[i];
    return NULL;
}

Res *rsrc_put(ResFile *rf, const char *type, int id, const MacName *name, const unsigned char *data, uint32_t len) {
    Res *x = *type ? rsrc_get(rf, type, id) : NULL;
    if (x) free(x->data);
    else {
        rf->v = xrealloc(rf->v, (size_t)(rf->n + 1) * sizeof *rf->v);
        x = &rf->v[rf->n++];
        memset(x, 0, sizeof *x);
        if (*type) memcpy(x->type, type, 4);
        x->id = id;
    }
    x->has_name = name != NULL;
    if (name) x->name = *name;
    x->data = xmalloc(len ? len : 1);
    if (len) memcpy(x->data, data, len);
    x->len = len;
    return x;
}

void rsrc_del(ResFile *rf, const char *type, int id) {
    Res *x = rsrc_get(rf, type, id);
    if (!x) return;
    free(x->data);
    int i = (int)(x - rf->v);
    memmove(&rf->v[i], &rf->v[i + 1], (size_t)(rf->n - i - 1) * sizeof *rf->v);
    rf->n--;
}

void rsrc_free(ResFile *rf) {
    for (int i = 0; i < rf->n; i++) free(rf->v[i].data);
    free(rf->v);
    memset(rf, 0, sizeof *rf);
}
