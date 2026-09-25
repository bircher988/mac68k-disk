/* hfs.c - the Hierarchical File System (HFS) of the 800K floppy, the Mac Plus
 * and everything after it.
 *
 * Layout (512-byte sectors, big-endian):
 *   sectors 0-1    boot blocks
 *   sector 2       master directory block (MDB)
 *   drVBMSt..      volume bitmap, one bit per allocation block (1 = in use)
 *   drAlBlSt..     allocation blocks, numbered from 0
 *   last-but-one   alternate MDB (a copy of the MDB)
 *
 * The MDB fields used here are listed below as MDB_*. Two special files live
 * in allocation blocks like any other file: the extents overflow file (extents
 * of forks that do not fit into the three extents of their catalog record)
 * and the catalog file. Both are B-trees (see btree.c); their first three
 * extents are in the MDB.
 *
 * Catalog keys: 0 key length, 1 reserved, 2 parent directory ID, 6 name
 * (Str31). Records (first byte = type):
 *   1 folder (70 bytes): 2 flags, 4 valence, 6 directory ID, 10 created,
 *     14 modified, 18 backed up, 22 Finder info (DInfo), 38 DXInfo, 54 reserved
 *   2 file (102 bytes): 2 flags, 3 version, 4 Finder info (FInfo), 20 file ID,
 *     24 first block (unused), 26 data logical length, 30 data physical
 *     length, 34 first block (unused), 36 resource logical length, 40
 *     resource physical length, 44 created, 48 modified, 52 backed up,
 *     56 FXInfo, 72 clump size, 74 data extents, 86 resource extents
 *   3 folder thread / 4 file thread (46 bytes): 2 reserved, 10 parent ID,
 *     14 name (Str31); key = (own ID, empty name)
 * Directory ID 1 is the parent of the root, 2 the root folder; the root's
 * own record is keyed (1, volume name). IDs from 16 on are for users.
 *
 * Extents keys: 0 key length (7), 1 fork (0 data, $FF resource), 2 file ID,
 * 6 first file allocation block of the record; data: three extents
 * (start block, block count). */
#include "btree.h"
#include "util.h"
#include "vol.h"
#include <stdlib.h>
#include <string.h>

#define BLK 512
#define MDB_OFF 1024

#define MDB_SIG       0x00
#define MDB_CRDATE    0x02
#define MDB_LSMOD     0x06
#define MDB_ATRB      0x0A
#define MDB_NMFLS     0x0C
#define MDB_VBMST     0x0E
#define MDB_NMALBLKS  0x12
#define MDB_ALBLKSIZ  0x14
#define MDB_CLPSIZ    0x18
#define MDB_ALBLST    0x1C
#define MDB_NXTCNID   0x1E
#define MDB_FREEBKS   0x22
#define MDB_VN        0x24
#define MDB_WRCNT     0x46
#define MDB_XTCLPSIZ  0x4A
#define MDB_CTCLPSIZ  0x4E
#define MDB_NMRTDIRS  0x52
#define MDB_FILCNT    0x54
#define MDB_DIRCNT    0x58
#define MDB_FNDRINFO  0x5C
#define MDB_EMBEDSIG  0x7C
#define MDB_XTFLSIZE  0x82
#define MDB_XTEXTREC  0x86
#define MDB_CTFLSIZE  0x92
#define MDB_CTEXTREC  0x96

#define REC_DIR     1
#define REC_FILE    2
#define REC_DTHREAD 3
#define REC_FTHREAD 4

#define CAT_FILE_ID 4
#define CAT_KEY_MAX 37
#define EXT_KEY_MAX 7

typedef struct { unsigned start, count; } Ext;
typedef struct { Ext *e; int n, cap; } ExtList;

typedef struct {
    uint32_t parid;
    MacName name;
    unsigned char *data;
    int dlen;
} CatRec;

typedef struct {
    unsigned char key[8];
    unsigned char data[12];
} ExtRec;

typedef struct {
    unsigned char *mdb;
    uint32_t nsect, blksize, spb;
    unsigned nmalblks, alblst, vbmst;
    unsigned char *bitmap;
    size_t bmbytes;
    CatRec *cat;
    int ncat, capcat;
    ExtRec *ext;
    int next, capext;
    int ext_dirty;
    unsigned char cat_hdr[BLK], ext_hdr[BLK];
} Hfs;

static Hfs *H(Volume *v) { return (Hfs *)v->fs; }

/* ---- volume bitmap ---- */

static int bm_get(Hfs *h, unsigned i) { return (h->bitmap[i / 8] >> (7 - i % 8)) & 1; }

static void bm_set(Hfs *h, unsigned start, unsigned count, int on) {
    for (unsigned i = start; i < start + count; i++) {
        if (on) h->bitmap[i / 8] |= (unsigned char)(0x80 >> (i % 8));
        else h->bitmap[i / 8] &= (unsigned char)~(0x80 >> (i % 8));
    }
}

static unsigned bm_free(Hfs *h) {
    unsigned f = 0;
    for (unsigned i = 0; i < h->nmalblks; i++) f += !bm_get(h, i);
    return f;
}

/* ---- extents ---- */

static void extlist_add(ExtList *l, unsigned start, unsigned count) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 8; l->e = xrealloc(l->e, (size_t)l->cap * sizeof *l->e); }
    l->e[l->n].start = start;
    l->e[l->n].count = count;
    l->n++;
}

static void extlist_add_rec(ExtList *l, const unsigned char *rec12) {
    for (int i = 0; i < 3; i++) {
        unsigned c = get16(rec12 + 4 * i + 2);
        if (c) extlist_add(l, get16(rec12 + 4 * i), c);
    }
}

static unsigned extlist_blocks(const ExtList *l) {
    unsigned n = 0;
    for (int i = 0; i < l->n; i++) n += l->e[i].count;
    return n;
}

/* All extents of a fork: the three of its catalog (or MDB) record, then those
 * of its records in the extents overflow file. */
static void fork_extents(Hfs *h, uint32_t fileid, int rsrc, const unsigned char *first3, ExtList *out) {
    memset(out, 0, sizeof *out);
    extlist_add_rec(out, first3);
    for (int i = 0; i < h->next; i++) {
        const unsigned char *k = h->ext[i].key;
        if (get32(k + 2) == fileid && k[1] == (rsrc ? 0xFF : 0x00)) extlist_add_rec(out, h->ext[i].data);
    }
}

static int extlist_valid(Hfs *h, const ExtList *l) {
    for (int i = 0; i < l->n; i++)
        if ((uint64_t)l->e[i].start + l->e[i].count > h->nmalblks) return 0;
    return 1;
}

static size_t block_off(Hfs *h, unsigned ab) { return ((size_t)h->alblst + (size_t)ab * h->spb) * BLK; }

static unsigned char *read_extents(Volume *v, const ExtList *l, uint32_t len) {
    Hfs *h = H(v);
    unsigned char *out = xcalloc(len ? len : 1, 1);
    uint32_t done = 0;
    for (int i = 0; i < l->n && done < len; i++) {
        for (unsigned b = 0; b < l->e[i].count && done < len; b++) {
            uint32_t take = len - done < h->blksize ? len - done : h->blksize;
            memcpy(out + done, v->img + block_off(h, l->e[i].start + b), take);
            done += take;
        }
    }
    return out;
}

static void write_extents(Volume *v, const ExtList *l, const unsigned char *data, uint32_t len) {
    Hfs *h = H(v);
    uint32_t done = 0;
    for (int i = 0; i < l->n; i++) {
        for (unsigned b = 0; b < l->e[i].count; b++) {
            unsigned char *dst = v->img + block_off(h, l->e[i].start + b);
            uint32_t take = done < len ? (len - done < h->blksize ? len - done : h->blksize) : 0;
            memset(dst, 0, h->blksize);
            if (take) memcpy(dst, data + done, take);
            done += take;
        }
    }
}

/* ---- catalog records ---- */

static int make_cat_key(unsigned char *out, uint32_t parid, const MacName *name) {
    out[0] = (unsigned char)(6 + name->len);
    out[1] = 0;
    put32(out + 2, parid);
    out[6] = name->len;
    memcpy(out + 7, name->s, name->len);
    return 7 + name->len;
}

static int cat_cmp(uint32_t pa, const MacName *na, uint32_t pb, const MacName *nb) {
    if (pa != pb) return pa < pb ? -1 : 1;
    return name_cmp(na->s, na->len, nb->s, nb->len);
}

static int rec_type(const CatRec *r) { return r->dlen > 0 ? r->data[0] : 0; }

static uint32_t dir_id(const CatRec *r) { return get32(r->data + 6); }

/* Inserts a record at its place in the existing order: before the first
 * record with a greater key. Existing records keep their relative order. */
static void cat_insert(Hfs *h, uint32_t parid, const MacName *name, const unsigned char *data, int dlen) {
    int pos = h->ncat;
    for (int i = 0; i < h->ncat; i++)
        if (cat_cmp(h->cat[i].parid, &h->cat[i].name, parid, name) > 0) { pos = i; break; }
    if (h->ncat == h->capcat) { h->capcat = h->capcat ? h->capcat * 2 : 64; h->cat = xrealloc(h->cat, (size_t)h->capcat * sizeof *h->cat); }
    memmove(&h->cat[pos + 1], &h->cat[pos], (size_t)(h->ncat - pos) * sizeof *h->cat);
    CatRec *r = &h->cat[pos];
    r->parid = parid;
    r->name = *name;
    r->data = xmalloc((size_t)dlen);
    memcpy(r->data, data, (size_t)dlen);
    r->dlen = dlen;
    h->ncat++;
}

static void cat_remove(Hfs *h, int i) {
    free(h->cat[i].data);
    memmove(&h->cat[i], &h->cat[i + 1], (size_t)(h->ncat - i - 1) * sizeof *h->cat);
    h->ncat--;
}

/* Index of the folder or file record (parid, name), or -1. */
static int cat_find(Hfs *h, uint32_t parid, const MacName *name) {
    for (int i = 0; i < h->ncat; i++) {
        int t = rec_type(&h->cat[i]);
        if ((t == REC_DIR || t == REC_FILE) && h->cat[i].parid == parid && cat_cmp(parid, &h->cat[i].name, parid, name) == 0)
            return i;
    }
    return -1;
}

static int cat_find_dir(Hfs *h, uint32_t dirid) {
    for (int i = 0; i < h->ncat; i++)
        if (rec_type(&h->cat[i]) == REC_DIR && h->cat[i].dlen >= 70 && dir_id(&h->cat[i]) == dirid) return i;
    return -1;
}

static int cat_find_thread(Hfs *h, uint32_t id, int type) {
    for (int i = 0; i < h->ncat; i++)
        if (h->cat[i].parid == id && h->cat[i].name.len == 0 && rec_type(&h->cat[i]) == type) return i;
    return -1;
}

static void touch_dir(Hfs *h, uint32_t dirid, uint32_t now) {
    int i = cat_find_dir(h, dirid);
    if (i >= 0) put32(h->cat[i].data + 14, now);
}

/* ---- opening ---- */

static unsigned char *read_btree_file(Volume *v, const char *what, const ExtList *l, uint32_t size, uint32_t *nnodes) {
    Hfs *h = H(v);
    if (size % BLK || size == 0 || !extlist_valid(h, l) || (uint64_t)extlist_blocks(l) * h->blksize < size)
        fail(v->path, "damaged HFS volume (bad extents of the %s file)", what);
    *nnodes = size / BLK;
    return read_extents(v, l, size);
}

void *hfs_open(Volume *v) {
    Hfs *h = xcalloc(1, sizeof *h);
    v->fs = h;
    unsigned char *m = v->img + MDB_OFF;
    h->mdb = m;
    if (get16(m + MDB_EMBEDSIG) == 0x482B) fail(v->path, "HFS+ volumes are not supported (only HFS and MFS)");
    h->nsect = (uint32_t)(v->size / BLK);
    h->blksize = get32(m + MDB_ALBLKSIZ);
    h->nmalblks = get16(m + MDB_NMALBLKS);
    h->alblst = get16(m + MDB_ALBLST);
    h->vbmst = get16(m + MDB_VBMST);
    if (h->blksize == 0 || h->blksize % BLK || h->nmalblks == 0 || h->vbmst < 3)
        fail(v->path, "damaged HFS volume (bad master directory block)");
    h->spb = h->blksize / BLK;
    size_t bmsect = (h->nmalblks + 4095) / 4096;
    if ((uint64_t)h->alblst + (uint64_t)h->nmalblks * h->spb > h->nsect || h->vbmst + bmsect > h->alblst)
        fail(v->path, "damaged HFS volume (allocation blocks extend past the end of the image)");
    h->bmbytes = bmsect * BLK;
    h->bitmap = xmalloc(h->bmbytes);
    memcpy(h->bitmap, v->img + (size_t)h->vbmst * BLK, h->bmbytes);

    const char *err;
    BtRec *recs;
    int n;
    uint32_t nnodes;

    /* extents overflow file */
    ExtList xl = {0};
    extlist_add_rec(&xl, m + MDB_XTEXTREC);
    unsigned char *xf = read_btree_file(v, "extents", &xl, get32(m + MDB_XTFLSIZE), &nnodes);
    if (bt_read(xf, nnodes, &recs, &n, &err)) fail(v->path, "damaged HFS volume (extents file: %s)", err);
    memcpy(h->ext_hdr, xf, BLK);
    h->ext = xcalloc((size_t)n + 1, sizeof *h->ext);
    for (int i = 0; i < n; i++) {
        if (recs[i].klen != 8 || recs[i].dlen < 12) fail(v->path, "damaged HFS volume (bad extents record)");
        memcpy(h->ext[i].key, recs[i].key, 8);
        memcpy(h->ext[i].data, recs[i].data, 12);
    }
    h->next = h->capext = n;
    free(recs);
    free(xf);
    free(xl.e);

    /* catalog file */
    ExtList cl;
    fork_extents(h, CAT_FILE_ID, 0, m + MDB_CTEXTREC, &cl);
    unsigned char *cf = read_btree_file(v, "catalog", &cl, get32(m + MDB_CTFLSIZE), &nnodes);
    if (bt_read(cf, nnodes, &recs, &n, &err)) fail(v->path, "damaged HFS volume (catalog: %s)", err);
    memcpy(h->cat_hdr, cf, BLK);
    h->cat = xcalloc((size_t)n + 1, sizeof *h->cat);
    h->capcat = n + 1;
    for (int i = 0; i < n; i++) {
        const unsigned char *k = recs[i].key;
        if (recs[i].klen < 7 || k[6] > 31 || 7 + k[6] > recs[i].klen || recs[i].dlen < 1)
            fail(v->path, "damaged HFS volume (bad catalog key)");
        CatRec *r = &h->cat[i];
        r->parid = get32(k + 2);
        r->name.len = k[6];
        memcpy(r->name.s, k + 7, k[6]);
        r->dlen = recs[i].dlen;
        r->data = xmalloc((size_t)r->dlen);
        memcpy(r->data, recs[i].data, (size_t)r->dlen);
        int t = r->data[0];
        if ((t == REC_DIR && r->dlen < 70) || (t == REC_FILE && r->dlen < 102) || ((t == REC_DTHREAD || t == REC_FTHREAD) && r->dlen < 46))
            fail(v->path, "damaged HFS volume (short catalog record)");
    }
    h->ncat = n;
    free(recs);
    free(cf);
    free(cl.e);
    return h;
}

/* ---- listing and reading ---- */

static void entry_from(const CatRec *r, Entry *e) {
    const unsigned char *d = r->data;
    memset(e, 0, sizeof *e);
    e->name = r->name;
    e->parent = r->parid;
    if (d[0] == REC_DIR) {
        e->is_dir = 1;
        e->valence = get16(d + 4);
        e->id = get32(d + 6);
        e->crdate = get32(d + 10);
        e->mddate = get32(d + 14);
        memcpy(e->finfo, d + 22, 16);
    } else {
        memcpy(e->finfo, d + 4, 16);
        e->id = get32(d + 20);
        e->dlen = get32(d + 26);
        e->dphys = get32(d + 30);
        e->rlen = get32(d + 36);
        e->rphys = get32(d + 40);
        e->crdate = get32(d + 44);
        e->mddate = get32(d + 48);
    }
}

int hfs_list(Volume *v, uint32_t dir, Entry **out) {
    Hfs *h = H(v);
    Entry *e = xcalloc((size_t)h->ncat + 1, sizeof *e);
    int n = 0;
    for (int i = 0; i < h->ncat; i++) {
        int t = rec_type(&h->cat[i]);
        if ((t == REC_DIR || t == REC_FILE) && h->cat[i].parid == dir) entry_from(&h->cat[i], &e[n++]);
    }
    *out = e;
    return n;
}

static int find_entry(Volume *v, const Entry *e) {
    int i = cat_find(H(v), e->parent, &e->name);
    if (i < 0) fail(v->path, "%s: not found", name_str(&e->name));
    return i;
}

void hfs_read_fork(Volume *v, const Entry *e, int rsrc, unsigned char **data, uint32_t *len) {
    Hfs *h = H(v);
    const unsigned char *d = h->cat[find_entry(v, e)].data;
    uint32_t n = get32(d + (rsrc ? 36 : 26));
    ExtList l;
    fork_extents(h, get32(d + 20), rsrc, d + (rsrc ? 86 : 74), &l);
    if (!extlist_valid(h, &l) || (uint64_t)extlist_blocks(&l) * h->blksize < n)
        fail(v->path, "damaged HFS volume (the extents of %s do not cover its %s fork)", name_str(&e->name), rsrc ? "resource" : "data");
    *data = read_extents(v, &l, n);
    *len = n;
    free(l.e);
}

/* ---- allocation ---- */

/* Allocates count blocks as one run if possible, else in up to three runs
 * (the largest free runs). Returns the number of extents, or -1. */
static int alloc_blocks(Hfs *h, unsigned count, Ext out[3]) {
    memset(out, 0, 3 * sizeof *out);
    if (count == 0) return 0;
    Ext best[3] = {{0, 0}, {0, 0}, {0, 0}};
    for (unsigned i = 0; i < h->nmalblks;) {
        if (bm_get(h, i)) { i++; continue; }
        unsigned j = i;
        while (j < h->nmalblks && !bm_get(h, j)) j++;
        if (j - i >= count) {
            out[0].start = i;
            out[0].count = count;
            bm_set(h, i, count, 1);
            return 1;
        }
        Ext run = {i, j - i};
        for (int k = 0; k < 3; k++) {
            if (run.count > best[k].count) { Ext t = best[k]; best[k] = run; run = t; }
        }
        i = j;
    }
    if (best[0].count + best[1].count + best[2].count < count) return -1;
    unsigned left = count;
    int n = 0;
    for (int k = 0; k < 3 && left; k++) {
        unsigned take = best[k].count < left ? best[k].count : left;
        out[n].start = best[k].start;
        out[n].count = take;
        bm_set(h, best[k].start, take, 1);
        left -= take;
        n++;
    }
    return n;
}

static unsigned blocks_for(Hfs *h, uint32_t len) { return (unsigned)((len + (uint64_t)h->blksize - 1) / h->blksize); }

static void put_extents(unsigned char *p, const Ext e[3]) {
    for (int i = 0; i < 3; i++) { put16(p + 4 * i, e[i].start); put16(p + 4 * i + 2, e[i].count); }
}

static void write_fork(Volume *v, const Ext e[3], const unsigned char *data, uint32_t len) {
    ExtList l = {0};
    for (int i = 0; i < 3; i++) if (e[i].count) extlist_add(&l, e[i].start, e[i].count);
    write_extents(v, &l, data, len);
    free(l.e);
}

void hfs_add_file(Volume *v, uint32_t dir, const MacFile *f) {
    Hfs *h = H(v);
    unsigned nd = blocks_for(h, f->dlen), nr = blocks_for(h, f->rlen), avail = bm_free(h);
    if (nd + nr > avail)
        fail(v->path, "disk full (%s needs %u KB, %u KB free)", name_str(&f->name), kb((uint64_t)(nd + nr) * h->blksize),
             (unsigned)((uint64_t)avail * h->blksize / 1024));
    Ext de[3], re[3];
    if (alloc_blocks(h, nd, de) < 0 || alloc_blocks(h, nr, re) < 0)
        fail(v->path, "not enough contiguous free space for %s (needs %u KB in at most 3 pieces per fork; the disk is fragmented)",
             name_str(&f->name), kb((uint64_t)(nd + nr) * h->blksize));
    write_fork(v, de, f->data, f->dlen);
    write_fork(v, re, f->rsrc, f->rlen);

    uint32_t cnid = get32(h->mdb + MDB_NXTCNID);
    put32(h->mdb + MDB_NXTCNID, cnid + 1);
    unsigned char d[102];
    memset(d, 0, sizeof d);
    d[0] = REC_FILE;
    memcpy(d + 4, f->type, 4);
    memcpy(d + 8, f->creator, 4);
    put16(d + 12, f->flags);
    put16(d + 14, (unsigned)f->v & 0xFFFF);
    put16(d + 16, (unsigned)f->h & 0xFFFF);
    put16(d + 18, (unsigned)f->fldr & 0xFFFF);
    put32(d + 20, cnid);
    put32(d + 26, f->dlen);
    put32(d + 30, nd * h->blksize);
    put32(d + 36, f->rlen);
    put32(d + 40, nr * h->blksize);
    put32(d + 44, f->crdate);
    put32(d + 48, f->mddate);
    put_extents(d + 74, de);
    put_extents(d + 86, re);
    cat_insert(h, dir, &f->name, d, (int)sizeof d);
    touch_dir(h, dir, mac_now());
}

void hfs_mkdir(Volume *v, uint32_t dir, const MacName *name) {
    Hfs *h = H(v);
    uint32_t now = mac_now();
    uint32_t cnid = get32(h->mdb + MDB_NXTCNID);
    put32(h->mdb + MDB_NXTCNID, cnid + 1);
    unsigned char d[70];
    memset(d, 0, sizeof d);
    d[0] = REC_DIR;
    put32(d + 6, cnid);
    put32(d + 10, now);
    put32(d + 14, now);
    cat_insert(h, dir, name, d, (int)sizeof d);
    unsigned char t[46];
    memset(t, 0, sizeof t);
    t[0] = REC_DTHREAD;
    put32(t + 10, dir);
    t[14] = name->len;
    memcpy(t + 15, name->s, name->len);
    MacName empty = {0, {0}};
    cat_insert(h, cnid, &empty, t, (int)sizeof t);
    touch_dir(h, dir, now);
}

static void free_fork(Hfs *h, const unsigned char *d, int rsrc) {
    ExtList l;
    fork_extents(h, get32(d + 20), rsrc, d + (rsrc ? 86 : 74), &l);
    for (int i = 0; i < l.n; i++)
        if ((uint64_t)l.e[i].start + l.e[i].count <= h->nmalblks) bm_set(h, l.e[i].start, l.e[i].count, 0);
    free(l.e);
}

void hfs_delete(Volume *v, const Entry *e) {
    Hfs *h = H(v);
    int i = find_entry(v, e);
    const unsigned char *d = h->cat[i].data;
    uint32_t parent = h->cat[i].parid;
    if (d[0] == REC_DIR) {
        uint32_t id = dir_id(&h->cat[i]);
        for (int k = 0; k < h->ncat; k++) {
            int t = rec_type(&h->cat[k]);
            if ((t == REC_DIR || t == REC_FILE) && h->cat[k].parid == id) fail(v->path, "%s: folder not empty", name_str(&e->name));
        }
        cat_remove(h, i);
        int th = cat_find_thread(h, id, REC_DTHREAD);
        if (th >= 0) cat_remove(h, th);
        if (get32(h->mdb + MDB_FNDRINFO) == id) put32(h->mdb + MDB_FNDRINFO, 0);
    } else {
        uint32_t id = get32(d + 20);
        free_fork(h, d, 0);
        free_fork(h, d, 1);
        for (int k = 0; k < h->next;) {
            if (get32(h->ext[k].key + 2) == id) {
                memmove(&h->ext[k], &h->ext[k + 1], (size_t)(h->next - k - 1) * sizeof *h->ext);
                h->next--;
                h->ext_dirty = 1;
            } else k++;
        }
        cat_remove(h, i);
        int th = cat_find_thread(h, id, REC_FTHREAD);
        if (th >= 0) cat_remove(h, th);
    }
    touch_dir(h, parent, mac_now());
}

/* ---- writing back ---- */

/* Makes the catalog file at least 'nodes' nodes long: extends its last
 * extent in place if the blocks behind it are free, else adds an extent
 * (the MDB holds three). Grows by the catalog clump size when there is room. */
static void grow_catalog(Volume *v, uint32_t nodes) {
    Hfs *h = H(v);
    unsigned char *m = h->mdb;
    uint32_t size = get32(m + MDB_CTFLSIZE);
    unsigned need = (unsigned)(((uint64_t)nodes * BLK - size + h->blksize - 1) / h->blksize);
    unsigned clump = get32(m + MDB_CTCLPSIZ) / h->blksize;
    unsigned want = clump > need ? clump : need;
    Ext e[3];
    int used = 0;
    for (int i = 0; i < 3; i++) {
        e[i].start = get16(m + MDB_CTEXTREC + 4 * i);
        e[i].count = get16(m + MDB_CTEXTREC + 4 * i + 2);
        if (e[i].count) used = i + 1;
    }
    for (int i = 0; i < h->next; i++)
        if (get32(h->ext[i].key + 2) == CAT_FILE_ID) fail(v->path, "catalog full (the catalog file cannot grow any further)");
    if (bm_free(h) < need)
        fail(v->path, "disk full (the catalog needs %u KB more, %u KB free)", kb((uint64_t)need * h->blksize),
             (unsigned)((uint64_t)bm_free(h) * h->blksize / 1024));
    unsigned end = used ? e[used - 1].start + e[used - 1].count : 0, run = 0;
    if (used)
        while (end + run < h->nmalblks && run < want && e[used - 1].count + run < 0xFFFF && !bm_get(h, end + run)) run++;
    if (used && run >= need) {
        bm_set(h, end, run, 1);
        e[used - 1].count += run;
    } else if (used < 3) {
        unsigned bstart = 0, blen = 0;
        for (unsigned i = 0; i < h->nmalblks && blen < want;) {
            if (bm_get(h, i)) { i++; continue; }
            unsigned j = i;
            while (j < h->nmalblks && !bm_get(h, j)) j++;
            if (j - i > blen) { bstart = i; blen = j - i; }
            i = j;
        }
        if (blen < need) fail(v->path, "catalog full (no room to extend the catalog file in one piece)");
        if (blen > want) blen = want;
        bm_set(h, bstart, blen, 1);
        e[used].start = bstart;
        e[used].count = blen;
        used++;
    } else {
        fail(v->path, "catalog full (the catalog file cannot grow any further)");
    }
    uint32_t total = 0;
    for (int i = 0; i < 3; i++) {
        put16(m + MDB_CTEXTREC + 4 * i, e[i].start);
        put16(m + MDB_CTEXTREC + 4 * i + 2, e[i].count);
        total += e[i].count;
    }
    put32(m + MDB_CTFLSIZE, total * h->blksize);
}

static void write_catalog(Volume *v) {
    Hfs *h = H(v);
    BtRec *recs = xcalloc((size_t)h->ncat + 1, sizeof *recs);
    unsigned char *keys = xmalloc((size_t)h->ncat * 40 + 1);
    for (int i = 0; i < h->ncat; i++) {
        recs[i].key = keys + (size_t)i * 40;
        recs[i].klen = make_cat_key(keys + (size_t)i * 40, h->cat[i].parid, &h->cat[i].name);
        recs[i].data = h->cat[i].data;
        recs[i].dlen = h->cat[i].dlen;
    }
    for (;;) {
        uint32_t nnodes = get32(h->mdb + MDB_CTFLSIZE) / BLK;
        uint32_t need = bt_nodes_needed(recs, h->ncat, CAT_KEY_MAX, nnodes);
        if (need <= nnodes) break;
        grow_catalog(v, need);
    }
    uint32_t size = get32(h->mdb + MDB_CTFLSIZE);
    unsigned char *file = xmalloc(size);
    if (bt_write(file, size / BLK, recs, h->ncat, CAT_KEY_MAX, h->cat_hdr)) fail(v->path, "catalog full");
    ExtList l;
    fork_extents(h, CAT_FILE_ID, 0, h->mdb + MDB_CTEXTREC, &l);
    write_extents(v, &l, file, size);
    free(l.e);
    free(file);
    free(keys);
    free(recs);
}

static void write_extents_file(Volume *v) {
    Hfs *h = H(v);
    BtRec *recs = xcalloc((size_t)h->next + 1, sizeof *recs);
    for (int i = 0; i < h->next; i++) {
        recs[i].key = h->ext[i].key;
        recs[i].klen = 8;
        recs[i].data = h->ext[i].data;
        recs[i].dlen = 12;
    }
    uint32_t size = get32(h->mdb + MDB_XTFLSIZE);
    unsigned char *file = xmalloc(size);
    if (bt_write(file, size / BLK, recs, h->next, EXT_KEY_MAX, h->ext_hdr)) fail(v->path, "extents file full");
    ExtList l = {0};
    extlist_add_rec(&l, h->mdb + MDB_XTEXTREC);
    write_extents(v, &l, file, size);
    free(l.e);
    free(file);
    free(recs);
}

void hfs_commit(Volume *v) {
    Hfs *h = H(v);
    unsigned char *m = h->mdb;
    /* folder valences and the volume's counts, recomputed from the catalog */
    unsigned rootfiles = 0, rootdirs = 0, files = 0, dirs = 0;
    for (int i = 0; i < h->ncat; i++) {
        int t = rec_type(&h->cat[i]);
        if (t == REC_FILE) { files++; rootfiles += h->cat[i].parid == ROOT_ID; }
        if (t == REC_DIR) {
            dirs++;
            rootdirs += h->cat[i].parid == ROOT_ID;
            uint32_t id = dir_id(&h->cat[i]);
            unsigned val = 0;
            for (int k = 0; k < h->ncat; k++) {
                int tk = rec_type(&h->cat[k]);
                if ((tk == REC_DIR || tk == REC_FILE) && h->cat[k].parid == id) val++;
            }
            put16(h->cat[i].data + 4, val);
        }
    }
    write_catalog(v);
    if (h->ext_dirty) write_extents_file(v);
    memcpy(v->img + (size_t)h->vbmst * BLK, h->bitmap, h->bmbytes);
    put16(m + MDB_NMFLS, rootfiles);
    put16(m + MDB_NMRTDIRS, rootdirs);
    put32(m + MDB_FILCNT, files);
    put32(m + MDB_DIRCNT, dirs ? dirs - 1 : 0);
    put16(m + MDB_FREEBKS, bm_free(h));
    put32(m + MDB_LSMOD, mac_now());
    put32(m + MDB_WRCNT, get32(m + MDB_WRCNT) + 1);
    put16(m + MDB_ATRB, get16(m + MDB_ATRB) | 0x0100);     /* unmounted cleanly */
    uint32_t alt = h->nsect - 2;
    if ((uint64_t)alt >= (uint64_t)h->alblst + (uint64_t)h->nmalblks * h->spb)
        memcpy(v->img + (size_t)alt * BLK, m, BLK);
}

void hfs_info(Volume *v, VolInfo *vi) {
    Hfs *h = H(v);
    unsigned char *m = h->mdb;
    vi->name.len = m[MDB_VN] > 27 ? 27 : m[MDB_VN];
    memcpy(vi->name.s, m + MDB_VN + 1, vi->name.len);
    vi->crdate = get32(m + MDB_CRDATE);
    vi->mddate = get32(m + MDB_LSMOD);
    vi->blksize = h->blksize;
    vi->nblocks = h->nmalblks;
    vi->freeblocks = bm_free(h);
    for (int i = 0; i < h->ncat; i++) {
        int t = rec_type(&h->cat[i]);
        vi->files += t == REC_FILE;
        vi->folders += t == REC_DIR;
    }
    if (vi->folders) vi->folders--;
    uint32_t blessed = get32(m + MDB_FNDRINFO);
    int b = blessed ? cat_find_dir(h, blessed) : -1;
    if (b >= 0) vi->blessed = h->cat[b].name;
}

/* A new volume with the geometry Apple's disk initialization uses: bitmap
 * from sector 3, 512-byte allocation blocks on floppies, extents and catalog
 * file of 1/128 of the disk each, the alternate MDB in the last-but-one
 * sector. */
void hfs_format(unsigned char *img, size_t size, const MacName *name) {
    uint32_t nsect = (uint32_t)(size / BLK), spb = 1;
    while ((nsect - 6) / spb > 65535) spb++;
    uint32_t blksize = spb * BLK;
    uint32_t est = (nsect - 5) / spb;
    uint32_t alblst = 3 + (est + 4095) / 4096;
    uint32_t nmalblks = (nsect - alblst - 2) / spb;
    uint32_t fblocks = (uint32_t)(size / 128 / blksize);
    if (fblocks < 1) fblocks = 1;
    uint32_t now = mac_now();

    unsigned char *m = img + MDB_OFF;
    put16(m + MDB_SIG, 0x4244);
    put32(m + MDB_CRDATE, now);
    put32(m + MDB_LSMOD, now);
    put16(m + MDB_ATRB, 0x0100);
    put16(m + MDB_VBMST, 3);
    put16(m + MDB_NMALBLKS, nmalblks);
    put32(m + MDB_ALBLKSIZ, blksize);
    put32(m + MDB_CLPSIZ, 4 * blksize);
    put16(m + MDB_ALBLST, alblst);
    put32(m + MDB_NXTCNID, 16);
    put16(m + MDB_FREEBKS, nmalblks - 2 * fblocks);
    m[MDB_VN] = name->len;
    memcpy(m + MDB_VN + 1, name->s, name->len);
    put32(m + MDB_XTCLPSIZ, fblocks * blksize);
    put32(m + MDB_CTCLPSIZ, fblocks * blksize);
    put32(m + MDB_XTFLSIZE, fblocks * blksize);
    put16(m + MDB_XTEXTREC, 0);
    put16(m + MDB_XTEXTREC + 2, fblocks);
    put32(m + MDB_CTFLSIZE, fblocks * blksize);
    put16(m + MDB_CTEXTREC, fblocks);
    put16(m + MDB_CTEXTREC + 2, fblocks);
    for (uint32_t i = 0; i < 2 * fblocks; i++) img[3 * BLK + i / 8] |= (unsigned char)(0x80 >> (i % 8));

    uint32_t fsize = fblocks * blksize;
    unsigned char *file = xmalloc(fsize);
    bt_write(file, fsize / BLK, NULL, 0, EXT_KEY_MAX, NULL);
    memcpy(img + (size_t)alblst * BLK, file, fsize);

    unsigned char k1[40], k2[40], root[70], thread[46];
    MacName empty = {0, {0}};
    memset(root, 0, sizeof root);
    root[0] = REC_DIR;
    put32(root + 6, ROOT_ID);
    put32(root + 10, now);
    put32(root + 14, now);
    memset(thread, 0, sizeof thread);
    thread[0] = REC_DTHREAD;
    put32(thread + 10, 1);
    thread[14] = name->len;
    memcpy(thread + 15, name->s, name->len);
    BtRec recs[2] = {
        {k1, make_cat_key(k1, 1, name), root, (int)sizeof root},
        {k2, make_cat_key(k2, ROOT_ID, &empty), thread, (int)sizeof thread},
    };
    bt_write(file, fsize / BLK, recs, 2, CAT_KEY_MAX, NULL);
    memcpy(img + ((size_t)alblst + fblocks * spb) * BLK, file, fsize);
    free(file);
    memcpy(img + (size_t)(nsect - 2) * BLK, m, BLK);
}
