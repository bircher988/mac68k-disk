/* mfs.c - the Macintosh File System (MFS): the flat file system of the 400K
 * floppy, the only one the 64K ROM of the Macintosh 128K and 512K can read.
 *
 * Layout (512-byte logical blocks, big-endian; Inside Macintosh II-119ff):
 *   blocks 0-1     boot blocks (zero on a disk that is not a startup disk)
 *   block 2        master directory block: volume information (64 bytes),
 *                  then the allocation block map, which runs on into block 3
 *   drDirSt..      file directory, drBlLen blocks
 *   drAlBlSt..     allocation blocks, numbered from 2
 *
 * Volume information: 0 drSigWord $D2D7, 2 drCrDate, 6 drLsBkUp, 10 drAtrb,
 * 12 drNmFls, 14 drDirSt, 16 drBlLen, 18 drNmAlBlks, 20 drAlBlkSiz,
 * 24 drClpSiz, 28 drAlBlSt, 30 drNxtFNum, 34 drFreeBks, 36 drVN (Str27).
 *
 * The block map has one 12-bit entry per allocation block, packed two to
 * three bytes, starting with block 2: 0 = free, 1 = last block of a file,
 * otherwise the number of the file's next block.
 *
 * A directory entry: 0 flFlags (bit 7 = in use), 1 flTyp, 2 flUsrWds (Finder
 * info, 16 bytes), 18 flFlNum, 22 flStBlk, 24 flLgLen, 28 flPyLen, 32 flRStBlk,
 * 34 flRLgLen, 38 flRPyLen, 42 flCrDat, 46 flMdDat, 50 flNam (Pascal string).
 * Entries are word-aligned and never cross a block boundary; the first byte
 * without bit 7 set ends the entries of a block.
 *
 * Folders: MFS itself is flat - every name is unique on the volume. The
 * Finder keeps its folders in the invisible file "DeskTop" (type FNDR,
 * creator ERIK): one resource 'FOBJ' per folder, its ID being the folder
 * number, and every file names its folder in fdFldr (Finder info +14;
 * 0 = the disk's own window). FOBJ 0 describes the disk itself, STR 0
 * ("Finder 1.0") is the version of the file; the Finder 5.3 asks to "repair"
 * a disk whose DeskTop file lacks either.
 *
 * An FOBJ, as the Finder 5.3 writes it (measured): 94 bytes, then the name
 * as a Pascal string.
 *    0 kind: 8 folder, 4 disk         2 icon position in the parent (v, h)
 *    8 $0083 folder, $0081 disk      10 $0100: icon view, position set
 *   12 parent folder (0 = the disk's window, -2 = the desktop)
 *   26 created   30 modified   34 $FFFF   36 $003E
 *   44 window (top, left, bottom, right)   52 $FFF8 $FFF0
 *   60 number of items (visible files and folders)
 * The other bytes are the Finder's working state (scroll positions, handles);
 * zero works. The Finder places new files itself but not folders, so a new
 * folder gets the next free place of its 64-pixel grid. */
#include "rsrc.h"
#include "util.h"
#include "vol.h"
#include <stdlib.h>
#include <string.h>

#define FOBJ_FIXED 94
#define FOLDER_BASE 0x10000u    /* Entry.id of folder n: FOLDER_BASE + (uint16)n */
#define GRID 64                 /* the Finder's icon grid */
#define INVISIBLE 0x4000        /* fdFlags */
#define INITED 0x0100           /* fdFlags: the icon has a position */

#define BLK 512
#define MDB_OFF 1024
#define ENTRY_FIXED 51

typedef struct {
    int blk;                    /* directory block (0 .. drBlLen-1) */
    unsigned char raw[ENTRY_FIXED + 256];
    int len;                    /* bytes, word-aligned */
} MfsEnt;

typedef struct {
    unsigned char *mdb;
    unsigned dirst, bllen, nmalblks, alblst, spb;
    uint32_t alblksiz;
    uint16_t *map;              /* map[i] is the entry of allocation block i + 2 */
    MfsEnt *ents;
    int n, cap;
    ResFile desk;               /* resources of the DeskTop file */
    int desk_state;             /* 0 no DeskTop file, 1 read, -1 damaged */
    int desk_dirty;
} Mfs;

static Mfs *M(Volume *v) { return (Mfs *)v->fs; }

static unsigned map_get(const unsigned char *raw, unsigned i) {
    unsigned o = i * 3 / 2;
    return (i & 1) ? ((raw[o] & 0x0F) << 8) | raw[o + 1] : (raw[o] << 4) | (raw[o + 1] >> 4);
}

static void map_put(unsigned char *raw, unsigned i, unsigned v) {
    unsigned o = i * 3 / 2;
    if (i & 1) { raw[o] = (unsigned char)((raw[o] & 0xF0) | (v >> 8)); raw[o + 1] = (unsigned char)v; }
    else { raw[o] = (unsigned char)(v >> 4); raw[o + 1] = (unsigned char)((raw[o + 1] & 0x0F) | ((v & 0x0F) << 4)); }
}

static void desk_load(Volume *v);

void *mfs_open(Volume *v) {
    Mfs *m = xcalloc(1, sizeof *m);
    unsigned char *d = v->img + MDB_OFF;
    m->mdb = d;
    m->dirst = get16(d + 14);
    m->bllen = get16(d + 16);
    m->nmalblks = get16(d + 18);
    m->alblksiz = get32(d + 20);
    m->alblst = get16(d + 28);
    size_t nsect = v->size / BLK;
    if (m->alblksiz == 0 || m->alblksiz % BLK || m->alblksiz > 0x100000 || m->bllen == 0 || m->dirst < 3
        || m->dirst + m->bllen > nsect || m->nmalblks == 0 || m->nmalblks > 4094)
        fail(v->path, "damaged MFS volume (bad master directory block)");
    m->spb = m->alblksiz / BLK;
    if (m->alblst + (size_t)m->nmalblks * m->spb > nsect)
        fail(v->path, "damaged MFS volume (allocation blocks extend past the end of the image)");
    unsigned mapend = (m->dirst < m->alblst ? m->dirst : m->alblst) * BLK;
    if (MDB_OFF + 64 + (m->nmalblks * 3 + 1) / 2 > mapend)
        fail(v->path, "damaged MFS volume (block map overlaps the directory)");
    m->map = xcalloc(m->nmalblks, sizeof *m->map);
    for (unsigned i = 0; i < m->nmalblks; i++) m->map[i] = (uint16_t)map_get(d + 64, i);

    for (unsigned b = 0; b < m->bllen; b++) {
        const unsigned char *blk = v->img + (size_t)(m->dirst + b) * BLK;
        int o = 0;
        while (o + ENTRY_FIXED <= BLK && (blk[o] & 0x80)) {
            int len = ENTRY_FIXED + blk[o + 50];
            if (o + len > BLK) fail(v->path, "damaged MFS volume (directory entry crosses a block boundary)");
            if (m->n == m->cap) { m->cap = m->cap ? m->cap * 2 : 32; m->ents = xrealloc(m->ents, m->cap * sizeof *m->ents); }
            MfsEnt *e = &m->ents[m->n++];
            memset(e, 0, sizeof *e);
            e->blk = (int)b;
            e->len = len + (len & 1);
            memcpy(e->raw, blk + o, (size_t)len);
            o += e->len;
        }
    }
    v->fs = m;
    desk_load(v);
    return m;
}

static void entry_from(const MfsEnt *me, Entry *e) {
    const unsigned char *r = me->raw;
    memset(e, 0, sizeof *e);
    e->name.len = r[50];
    memcpy(e->name.s, r + 51, r[50]);
    e->id = get32(r + 18);
    e->parent = ROOT_ID;
    memcpy(e->finfo, r + 2, 16);
    e->dlen = get32(r + 24);
    e->dphys = get32(r + 28);
    e->rlen = get32(r + 34);
    e->rphys = get32(r + 38);
    e->crdate = get32(r + 42);
    e->mddate = get32(r + 46);
}

static int find_slot(Mfs *m, uint32_t fnum) {
    for (int i = 0; i < m->n; i++) if (get32(m->ents[i].raw + 18) == fnum) return i;
    return -1;
}

/* ---- the Finder's folders ---- */

static int is_desktop(const unsigned char *r) {
    return !memcmp(r + 2, "FNDR", 4) && name_cmp(r + 51, r[50], (const unsigned char *)"DeskTop", 7) == 0;
}

static int desk_slot(Mfs *m) {
    for (int i = 0; i < m->n; i++) if (is_desktop(m->ents[i].raw)) return i;
    return -1;
}

static void desk_load(Volume *v) {
    Mfs *m = M(v);
    int s = desk_slot(m);
    if (s < 0) return;
    Entry e;
    entry_from(&m->ents[s], &e);
    unsigned char *d;
    uint32_t n;
    mfs_read_fork(v, &e, 1, &d, &n);
    m->desk_state = rsrc_parse(d, n, &m->desk) ? -1 : 1;
    free(d);
}

/* The FOBJ of folder id (not the disk's), NULL if there is none. */
static Res *folder_res(Mfs *m, int id) {
    if (m->desk_state != 1 || id == 0) return NULL;
    Res *r = rsrc_get(&m->desk, "FOBJ", id);
    return r && r->len > FOBJ_FIXED && get16(r->data) == 8 ? r : NULL;
}

/* Folder of a file / of a folder; 0 (the disk's window) for unknown ones. */
static int file_folder(Mfs *m, const unsigned char *raw) {
    int f = (int16_t)get16(raw + 16);
    return folder_res(m, f) ? f : 0;
}

static int folder_parent(Mfs *m, const Res *r) {
    int p = (int16_t)get16(r->data + 12);
    return p != r->id && folder_res(m, p) ? p : 0;
}

static uint32_t folder_dir(int id) { return id == 0 ? ROOT_ID : FOLDER_BASE + (uint16_t)id; }
static int dir_folder(uint32_t dir) { return dir == ROOT_ID ? 0 : (int16_t)(uint16_t)(dir - FOLDER_BASE); }

static void folder_name(const Res *r, MacName *n) {
    unsigned l = r->data[FOBJ_FIXED];
    if (FOBJ_FIXED + 1 + l > r->len) l = r->len - FOBJ_FIXED - 1;
    n->len = (unsigned char)l;
    memcpy(n->s, r->data + FOBJ_FIXED + 1, l);
}

/* Items in a folder (0: the disk's window); invisible files only if all. */
static unsigned folder_items(Mfs *m, int id, int all) {
    unsigned n = 0;
    for (int i = 0; i < m->n; i++)
        if (file_folder(m, m->ents[i].raw) == id && (all || !(get16(m->ents[i].raw + 10) & INVISIBLE))) n++;
    for (int i = 0; m->desk_state == 1 && i < m->desk.n; i++) {
        Res *r = &m->desk.v[i];
        if (folder_res(m, r->id) == r && folder_parent(m, r) == id) n++;
    }
    return n;
}

static void folder_entry(Mfs *m, const Res *r, Entry *e) {
    memset(e, 0, sizeof *e);
    e->is_dir = 1;
    folder_name(r, &e->name);
    e->id = folder_dir(r->id);
    e->parent = folder_dir(folder_parent(m, r));
    e->crdate = get32(r->data + 26);
    e->mddate = get32(r->data + 30);
    e->valence = folder_items(m, r->id, 1);
}

int mfs_list(Volume *v, uint32_t dir, Entry **out) {
    Mfs *m = M(v);
    int want = dir_folder(dir), k = 0;
    Entry *e = xcalloc((size_t)m->n + (size_t)m->desk.n + 1, sizeof *e);
    for (int i = 0; i < m->n; i++) {
        if (file_folder(m, m->ents[i].raw) != want) continue;
        entry_from(&m->ents[i], &e[k]);
        e[k++].parent = dir;
    }
    for (int i = 0; m->desk_state == 1 && i < m->desk.n; i++) {
        Res *r = &m->desk.v[i];
        if (folder_res(m, r->id) == r && folder_parent(m, r) == want) folder_entry(m, r, &e[k++]);
    }
    *out = e;
    return k;
}

/* A file of that name anywhere on the volume (MFS names are unique). */
int mfs_find_file(Volume *v, const MacName *name, Entry *e) {
    Mfs *m = M(v);
    for (int i = 0; i < m->n; i++) {
        const unsigned char *r = m->ents[i].raw;
        if (name_cmp(r + 51, r[50], name->s, name->len)) continue;
        entry_from(&m->ents[i], e);
        e->parent = folder_dir(file_folder(m, r));
        return 1;
    }
    return 0;
}

/* The allocation blocks of a fork, following the block map. */
static void fork_chain(Volume *v, unsigned start, uint32_t len, unsigned **blocks, unsigned *n) {
    Mfs *m = M(v);
    *blocks = NULL;
    *n = 0;
    if (start == 0 || len == 0) return;
    unsigned *b = xcalloc(m->nmalblks, sizeof *b);
    unsigned k = 0, cur = start;
    for (;;) {
        if (cur < 2 || cur >= m->nmalblks + 2 || k >= m->nmalblks)
            fail(v->path, "damaged MFS volume (bad block chain)");
        b[k++] = cur;
        unsigned next = m->map[cur - 2];
        if (next == 1 || next == 0xFFF) break;
        if (next == 0) fail(v->path, "damaged MFS volume (block chain runs into a free block)");
        cur = next;
    }
    if ((uint64_t)k * m->alblksiz < len) fail(v->path, "damaged MFS volume (fork shorter than its length)");
    *blocks = b;
    *n = k;
}

void mfs_read_fork(Volume *v, const Entry *e, int rsrc, unsigned char **data, uint32_t *len) {
    Mfs *m = M(v);
    int slot = find_slot(m, e->id);
    if (slot < 0) fail(v->path, "file not found");
    const unsigned char *r = m->ents[slot].raw;
    unsigned start = get16(r + (rsrc ? 32 : 22));
    uint32_t n = get32(r + (rsrc ? 34 : 24));
    unsigned *blocks, nb;
    fork_chain(v, start, n, &blocks, &nb);
    unsigned char *out = xmalloc(n ? n : 1);
    uint32_t done = 0;
    for (unsigned i = 0; i < nb && done < n; i++) {
        size_t off = ((size_t)m->alblst + (size_t)(blocks[i] - 2) * m->spb) * BLK;
        uint32_t take = n - done < m->alblksiz ? n - done : m->alblksiz;
        memcpy(out + done, v->img + off, take);
        done += take;
    }
    free(blocks);
    *data = out;
    *len = n;
}

static unsigned free_blocks(Mfs *m) {
    unsigned f = 0;
    for (unsigned i = 0; i < m->nmalblks; i++) if (m->map[i] == 0) f++;
    return f;
}

static unsigned blocks_for(Mfs *m, uint32_t len) { return (unsigned)((len + (uint64_t)m->alblksiz - 1) / m->alblksiz); }

/* Allocates and fills the blocks of one fork: one contiguous run if there is
 * one, otherwise the free blocks in ascending order. Returns the first block. */
static unsigned write_fork(Volume *v, const unsigned char *data, uint32_t len) {
    Mfs *m = M(v);
    unsigned need = blocks_for(m, len);
    if (need == 0) return 0;
    unsigned *list = xcalloc(need, sizeof *list), k = 0;
    for (unsigned s = 0; s + need <= m->nmalblks && k == 0; s++) {
        unsigned r = 0;
        while (r < need && m->map[s + r] == 0) r++;
        if (r == need) { for (unsigned i = 0; i < need; i++) list[k++] = s + i; }
        else s += r;
    }
    for (unsigned i = 0; i < m->nmalblks && k < need; i++) if (m->map[i] == 0) list[k++] = i;
    if (k < need) fail(v->path, "disk full");
    for (unsigned i = 0; i < need; i++) {
        m->map[list[i]] = (uint16_t)(i + 1 < need ? list[i + 1] + 2 : 1);
        size_t off = ((size_t)m->alblst + (size_t)list[i] * m->spb) * BLK;
        uint32_t pos = i * m->alblksiz;
        uint32_t take = len - pos < m->alblksiz ? len - pos : m->alblksiz;
        memset(v->img + off, 0, m->alblksiz);
        memcpy(v->img + off, data + pos, take);
    }
    unsigned first = list[0] + 2;
    free(list);
    return first;
}

static void add_entry(Volume *v, const MacFile *f, int fldr) {
    Mfs *m = M(v);
    int len = ENTRY_FIXED + f->name.len;
    len += len & 1;
    int blk = -1;
    for (unsigned b = 0; b < m->bllen && blk < 0; b++) {
        int used = 0;
        for (int i = 0; i < m->n; i++) if (m->ents[i].blk == (int)b) used += m->ents[i].len;
        if (used + len <= BLK) blk = (int)b;
    }
    if (blk < 0) fail(v->path, "file directory full (%d files)", m->n);
    unsigned need = blocks_for(m, f->dlen) + blocks_for(m, f->rlen), avail = free_blocks(m);
    if (need > avail)
        fail(v->path, "disk full (%s needs %u KB, %u KB free)", name_str(&f->name), kb((uint64_t)need * m->alblksiz),
             (unsigned)((uint64_t)avail * m->alblksiz / 1024));

    uint32_t fnum = get32(m->mdb + 30);
    if (m->n == m->cap) { m->cap = m->cap ? m->cap * 2 : 32; m->ents = xrealloc(m->ents, m->cap * sizeof *m->ents); }
    MfsEnt *e = &m->ents[m->n++];
    memset(e, 0, sizeof *e);
    e->blk = blk;
    e->len = len;
    unsigned char *r = e->raw;
    r[0] = 0x80;
    memcpy(r + 2, f->type, 4);
    memcpy(r + 6, f->creator, 4);
    put16(r + 10, f->flags);
    put16(r + 12, (unsigned)f->v & 0xFFFF);
    put16(r + 14, (unsigned)f->h & 0xFFFF);
    put16(r + 16, (unsigned)fldr & 0xFFFF);
    put32(r + 18, fnum);
    put16(r + 22, write_fork(v, f->data, f->dlen));
    put32(r + 24, f->dlen);
    put32(r + 28, blocks_for(m, f->dlen) * m->alblksiz);
    put16(r + 32, write_fork(v, f->rsrc, f->rlen));
    put32(r + 34, f->rlen);
    put32(r + 38, blocks_for(m, f->rlen) * m->alblksiz);
    put32(r + 42, f->crdate);
    put32(r + 46, f->mddate);
    r[50] = f->name.len;
    memcpy(r + 51, f->name.s, f->name.len);
    put32(m->mdb + 30, fnum + 1);
}

void mfs_add_file(Volume *v, uint32_t dir, const MacFile *f) {
    Mfs *m = M(v);
    add_entry(v, f, dir == ROOT_ID ? f->fldr : dir_folder(dir));
    if (m->desk_state == 1) m->desk_dirty = 1;
}

static void free_chain(Volume *v, unsigned start, uint32_t len) {
    Mfs *m = M(v);
    unsigned *blocks, nb;
    fork_chain(v, start, len ? len : 1, &blocks, &nb);
    for (unsigned i = 0; i < nb; i++) m->map[blocks[i] - 2] = 0;
    free(blocks);
}

void mfs_delete(Volume *v, const Entry *e) {
    Mfs *m = M(v);
    if (e->is_dir) {
        int id = dir_folder(e->id);
        if (folder_items(m, id, 1)) fail(v->path, "%s: folder not empty", name_str(&e->name));
        rsrc_del(&m->desk, "FOBJ", id);
        m->desk_dirty = 1;
        return;
    }
    int slot = find_slot(m, e->id);
    if (slot < 0) fail(v->path, "file not found");
    const unsigned char *r = m->ents[slot].raw;
    free_chain(v, get16(r + 22), get32(r + 24));
    free_chain(v, get16(r + 32), get32(r + 34));
    int desktop = is_desktop(r);
    memmove(&m->ents[slot], &m->ents[slot + 1], (size_t)(m->n - slot - 1) * sizeof *m->ents);
    m->n--;
    if (desktop) {                  /* the folders go with it, as in the Finder */
        rsrc_free(&m->desk);
        m->desk_state = 0;
        m->desk_dirty = 0;
    } else if (m->desk_state == 1) m->desk_dirty = 1;
}

/* A new FOBJ: kind 8 (folder) or 4 (disk), see the top of this file. */
static void fobj_make(unsigned char *d, unsigned kind, int parent, int pv, int ph, const int rect[4],
                      uint32_t date, const MacName *name) {
    memset(d, 0, FOBJ_FIXED);
    put16(d, kind);
    put16(d + 2, (unsigned)pv & 0xFFFF);
    put16(d + 4, (unsigned)ph & 0xFFFF);
    put16(d + 8, kind == 8 ? 0x0083 : 0x0081);
    put16(d + 10, 0x0100);
    put16(d + 12, (unsigned)parent & 0xFFFF);
    put32(d + 26, date);
    put32(d + 30, date);
    put16(d + 34, 0xFFFF);
    put16(d + 36, 0x003E);
    for (int i = 0; i < 4; i++) put16(d + 44 + 2 * i, (unsigned)rect[i]);
    put16(d + 52, 0xFFF8);
    put16(d + 54, 0xFFF0);
    d[FOBJ_FIXED] = name->len;
    memcpy(d + FOBJ_FIXED + 1, name->s, name->len);
}

/* The first place of the grid in folder id that no folder and no placed file
 * of that folder uses: rows of six icons, as in a new window. */
static void free_place(Mfs *m, int id, int *pv, int *ph) {
    for (int k = 0;; k++) {
        int v = k / 6 * GRID, h = k % 6 * GRID, used = 0;
        for (int i = 0; i < m->n && !used; i++) {
            const unsigned char *r = m->ents[i].raw;
            if (file_folder(m, r) != id || !(get16(r + 10) & INITED)) continue;
            int dv = (int16_t)get16(r + 12) - v, dh = (int16_t)get16(r + 14) - h;
            used = dv > -GRID / 2 && dv < GRID / 2 && dh > -GRID / 2 && dh < GRID / 2;
        }
        for (int i = 0; i < m->desk.n && !used; i++) {
            Res *r = &m->desk.v[i];
            if (folder_res(m, r->id) != r || folder_parent(m, r) != id) continue;
            int dv = (int16_t)get16(r->data + 2) - v, dh = (int16_t)get16(r->data + 4) - h;
            used = dv > -GRID / 2 && dv < GRID / 2 && dh > -GRID / 2 && dh < GRID / 2;
        }
        if (!used) { *pv = v; *ph = h; return; }
    }
}

void mfs_mkdir(Volume *v, uint32_t dir, const MacName *name) {
    Mfs *m = M(v);
    if (m->desk_state < 0)
        fail(v->path, "the Finder's DeskTop file is damaged; remove it (mac68k-disk rm %s DeskTop), "
             "the Finder then makes a new one - without the folders", v->path);
    m->desk_state = 1;
    uint32_t now = mac_now();
    unsigned char d[FOBJ_FIXED + 256];
    if (!rsrc_get(&m->desk, "STR ", 0))
        rsrc_put(&m->desk, "STR ", 0, NULL, (const unsigned char *)"\012Finder 1.0", 11);
    if (!rsrc_get(&m->desk, "FOBJ", 0)) {
        VolInfo vi;
        memset(&vi, 0, sizeof vi);
        mfs_info(v, &vi);
        static const int disk_rect[4] = {62, 14, 250, 418};
        fobj_make(d, 4, -2, 28, 456, disk_rect, now, &vi.name);     /* top right of the desktop */
        rsrc_put(&m->desk, "FOBJ", 0, &vi.name, d, FOBJ_FIXED + 1 + vi.name.len);
    }
    int id = 1, nfolders = 0;
    for (;; id++) {
        if (id > 32767) fail(v->path, "no free folder number");
        int used = rsrc_get(&m->desk, "FOBJ", id) != NULL;
        for (int i = 0; i < m->n && !used; i++) used = (int16_t)get16(m->ents[i].raw + 16) == id;
        if (!used) break;
    }
    for (int i = 0; i < m->desk.n; i++) if (folder_res(m, m->desk.v[i].id) == &m->desk.v[i]) nfolders++;
    int parent = dir_folder(dir), pv, ph, k = nfolders % 6;
    free_place(m, parent, &pv, &ph);
    int rect[4] = {82 + 20 * k, 24 + 10 * k, 270 + 20 * k, 428 + 10 * k};
    fobj_make(d, 8, parent, pv, ph, rect, now, name);
    rsrc_put(&m->desk, "FOBJ", id, name, d, FOBJ_FIXED + 1 + name->len);
    m->desk_dirty = 1;
}

/* Writes the DeskTop file back, with the item counts brought up to date;
 * creates it if the disk has none yet. */
static void desk_write(Volume *v) {
    Mfs *m = M(v);
    for (int i = 0; i < m->desk.n; i++) {
        Res *r = &m->desk.v[i];
        if (!memcmp(r->type, "FOBJ", 4) && r->len > FOBJ_FIXED && (r->id == 0 || folder_res(m, r->id) == r))
            put16(r->data + 60, folder_items(m, r->id, 0));
    }
    uint32_t len;
    unsigned char *fork = rsrc_build(&m->desk, &len);
    int s = desk_slot(m);
    if (s < 0) {
        MacFile f;
        memset(&f, 0, sizeof f);
        f.name.len = 7;
        memcpy(f.name.s, "DeskTop", 7);
        memcpy(f.type, "FNDR", 4);
        memcpy(f.creator, "ERIK", 4);
        f.flags = INVISIBLE;
        f.rsrc = fork;
        f.rlen = len;
        f.crdate = f.mddate = mac_now();
        add_entry(v, &f, 0);
    } else {
        unsigned char *r = m->ents[s].raw;
        free_chain(v, get16(r + 32), get32(r + 34));
        unsigned need = blocks_for(m, len), avail = free_blocks(m);
        if (need > avail) fail(v->path, "disk full (the Finder's DeskTop file needs %u KB)", kb((uint64_t)need * m->alblksiz));
        put16(r + 32, write_fork(v, fork, len));
        put32(r + 34, len);
        put32(r + 38, need * m->alblksiz);
        put32(r + 46, mac_now());
    }
    free(fork);
    m->desk_dirty = 0;
}

void mfs_commit(Volume *v) {
    Mfs *m = M(v);
    if (m->desk_dirty) desk_write(v);
    for (unsigned i = 0; i < m->nmalblks; i++) map_put(m->mdb + 64, i, m->map[i]);
    for (unsigned b = 0; b < m->bllen; b++) {
        unsigned char *blk = v->img + (size_t)(m->dirst + b) * BLK;
        int o = 0;
        memset(blk, 0, BLK);
        for (int i = 0; i < m->n; i++) {
            if (m->ents[i].blk != (int)b) continue;
            memcpy(blk + o, m->ents[i].raw, (size_t)m->ents[i].len);
            o += m->ents[i].len;
        }
    }
    put16(m->mdb + 12, (unsigned)m->n);
    put16(m->mdb + 34, free_blocks(m));
}

void mfs_info(Volume *v, VolInfo *vi) {
    Mfs *m = M(v);
    vi->name.len = m->mdb[36] > 27 ? 27 : m->mdb[36];
    memcpy(vi->name.s, m->mdb + 37, vi->name.len);
    vi->crdate = get32(m->mdb + 2);
    vi->blksize = m->alblksiz;
    vi->nblocks = m->nmalblks;
    vi->freeblocks = free_blocks(m);
    vi->files = (unsigned)m->n;
    vi->dir_size = m->bllen * BLK;
    for (int i = 0; i < m->n; i++) vi->dir_used += (unsigned)m->ents[i].len;
    for (int i = 0; m->desk_state == 1 && i < m->desk.n; i++)
        if (folder_res(m, m->desk.v[i].id) == &m->desk.v[i]) vi->folders++;
    for (int i = 0; i < m->n; i++) {           /* the folder with the System in it */
        const unsigned char *r = m->ents[i].raw;
        if (memcmp(r + 2, "ZSYS", 4) || name_cmp(r + 51, r[50], (const unsigned char *)"System", 6)) continue;
        Res *f = folder_res(m, file_folder(m, r));
        if (f) folder_name(f, &vi->blessed);
    }
}

/* A new 400K volume with the geometry of Apple's own 400K disks: directory
 * in blocks 4-15, 391 allocation blocks of 1 KB from block 16. */
void mfs_format(unsigned char *img, size_t size, const MacName *name) {
    unsigned char *d = img + MDB_OFF;
    unsigned nsect = (unsigned)(size / BLK);
    unsigned dirst = 4, bllen = 12, alblst = dirst + bllen;
    unsigned nmalblks = (nsect - alblst - 2) / 2;
    uint32_t now = mac_now();
    put16(d + 0, 0xD2D7);
    put32(d + 2, now);
    put16(d + 14, dirst);
    put16(d + 16, bllen);
    put16(d + 18, nmalblks);
    put32(d + 20, 1024);
    put32(d + 24, 8192);
    put16(d + 28, alblst);
    put32(d + 30, 1);
    put16(d + 34, nmalblks);
    d[36] = name->len;
    memcpy(d + 37, name->s, name->len);
}
