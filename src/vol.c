/* vol.c - opening, creating and saving disk images; dispatch to MFS or HFS. */
#include "vol.h"
#include "util.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define BLK 512

unsigned kb(uint64_t bytes) { return (unsigned)((bytes + 1023) / 1024); }

/* DiskCopy 4.2, the usual container of old Mac floppies (named .image, .img,
 * sometimes .dmg): an 84-byte header in front of the sectors, and behind them
 * the sectors' tag data (12 bytes each) or nothing.
 *    0 disk name (Pascal string in 64 bytes)   64 data size   68 tag size
 *   72 data checksum   76 tag checksum   80 disk format   81 format byte
 *   82 $0100
 * A checksum: every big-endian word is added, then the 32-bit sum rotated
 * right by one bit. The tag checksum leaves out the first sector's 12 bytes.
 * (Both rules checked against images made by DiskCopy.) */
#define DC42_HEADER 84
#define TAG 12

static int is_diskcopy42(const unsigned char *d, size_t n) {
    if (n < DC42_HEADER || d[82] != 0x01 || d[83] != 0x00 || d[0] == 0 || d[0] > 63) return 0;
    uint64_t data = get32(d + 64), tags = get32(d + 68);
    return data % BLK == 0 && DC42_HEADER + data + tags == n;
}

static uint32_t dc42_sum(const unsigned char *d, size_t n) {
    uint32_t s = 0;
    for (size_t i = 0; i + 1 < n; i += 2) {
        s += (uint32_t)d[i] << 8 | d[i + 1];
        s = s >> 1 | s << 31;
    }
    return s;
}

static uint32_t dc42_tag_sum(const unsigned char *tags, size_t n) {
    return n > TAG ? dc42_sum(tags + TAG, n - TAG) : 0;
}

Volume *vol_open(const char *path) {
    Volume *v = xcalloc(1, sizeof *v);
    v->path = path;
    if (read_file(path, &v->img, &v->size)) {
        if (errno == ENOENT) fail(path, "no such file (create a disk with: mac68k-disk new %s)", path);
        fail(path, "cannot read: %s", strerror(errno));
    }
    if (is_diskcopy42(v->img, v->size)) {
        v->file = v->img;
        v->file_size = v->size;
        v->img = v->file + DC42_HEADER;
        v->size = get32(v->file + 64);
        v->sums_ok = get32(v->file + 72) == dc42_sum(v->img, v->size)
                     && get32(v->file + 76) == dc42_tag_sum(v->img + v->size, get32(v->file + 68));
        v->before = xcalloc(v->size ? v->size : 1, 1);
        memcpy(v->before, v->img, v->size);
    }
    if (v->size < 3 * BLK) fail(path, "too small for a Mac disk image");
    if (v->size % BLK) fail(path, "not a disk image (neither a DiskCopy 4.2 image nor a multiple of 512 bytes)");
    unsigned sig = get16(v->img + 1024);
    if (sig == 0xD2D7) { v->kind = FS_MFS; v->fs = mfs_open(v); }
    else if (sig == 0x4244) { v->kind = FS_HFS; v->fs = hfs_open(v); }
    else if (sig == 0x482B) fail(path, "HFS+ volumes are not supported (only HFS and MFS)");
    else if (get16(v->img) == 0x4552) fail(path, "partitioned (hard disk) images are not supported yet, only floppy-style volumes");
    else fail(path, "no MFS or HFS volume found");
    return v;
}

void vol_save(Volume *v) {
    if (v->kind == FS_MFS) mfs_commit(v);
    else hfs_commit(v);
    if (v->file) {
        /* A tag is the file system's note on its sector for a disk scavenger
         * (file number, fork, sequence, date): where a sector changed, it no
         * longer holds and is cleared. */
        unsigned char *tags = v->img + v->size;
        size_t ntags = get32(v->file + 68);
        for (size_t s = 0; (s + 1) * TAG <= ntags && (s + 1) * BLK <= v->size; s++)
            if (memcmp(v->img + s * BLK, v->before + s * BLK, BLK)) memset(tags + s * TAG, 0, TAG);
        put32(v->file + 72, dc42_sum(v->img, v->size));
        put32(v->file + 76, dc42_tag_sum(tags, ntags));
        if (write_file_atomic(v->path, v->file, v->file_size)) fail(v->path, "cannot write: %s", strerror(errno));
        memcpy(v->before, v->img, v->size);
        v->sums_ok = 1;
        return;
    }
    if (write_file_atomic(v->path, v->img, v->size)) fail(v->path, "cannot write: %s", strerror(errno));
}

void vol_create(const char *path, FsKind kind, size_t size, const MacName *name) {
    unsigned char *img = xcalloc(size, 1);
    if (kind == FS_MFS) mfs_format(img, size, name);
    else hfs_format(img, size, name);
    if (write_file_atomic(path, img, size)) fail(path, "cannot write: %s", strerror(errno));
    free(img);
}

Volume *vol_new(const char *path, FsKind kind, size_t size, const MacName *name) {
    Volume *v = xcalloc(1, sizeof *v);
    v->path = path;
    v->size = size;
    v->kind = kind;
    v->img = xcalloc(size, 1);
    if (kind == FS_MFS) { mfs_format(v->img, size, name); v->fs = mfs_open(v); }
    else { hfs_format(v->img, size, name); v->fs = hfs_open(v); }
    return v;
}

int vol_max_name(const Volume *v) { return v->kind == FS_MFS ? 63 : 31; }

static int entry_cmp(const void *a, const void *b) {
    const Entry *x = a, *y = b;
    int c = name_cmp(x->name.s, x->name.len, y->name.s, y->name.len);
    if (c) return c;
    return memcmp(x->name.s, y->name.s, x->name.len < y->name.len ? x->name.len : y->name.len);
}

int vol_list(Volume *v, uint32_t dir, Entry **out) {
    int n = v->kind == FS_MFS ? mfs_list(v, dir, out) : hfs_list(v, dir, out);
    qsort(*out, (size_t)n, sizeof **out, entry_cmp);
    return n;
}

int vol_find(Volume *v, uint32_t dir, const MacName *name, Entry *e) {
    Entry *list;
    int n = v->kind == FS_MFS ? mfs_list(v, dir, &list) : hfs_list(v, dir, &list), found = 0;
    for (int i = 0; i < n && !found; i++) {
        if (name_cmp(list[i].name.s, list[i].name.len, name->s, name->len) == 0) { *e = list[i]; found = 1; }
    }
    free(list);
    return found;
}

int vol_find_name(Volume *v, uint32_t dir, const MacName *name, Entry *e) {
    if (v->kind == FS_MFS && mfs_find_file(v, name, e)) return 1;
    return vol_find(v, dir, name, e);
}

void vol_read_fork(Volume *v, const Entry *e, int rsrc, unsigned char **data, uint32_t *len) {
    if (v->kind == FS_MFS) mfs_read_fork(v, e, rsrc, data, len);
    else hfs_read_fork(v, e, rsrc, data, len);
}

void vol_add_file(Volume *v, uint32_t dir, const MacFile *f) {
    if (v->kind == FS_MFS) mfs_add_file(v, dir, f);
    else hfs_add_file(v, dir, f);
}

void vol_delete(Volume *v, const Entry *e) {
    if (v->kind == FS_MFS) mfs_delete(v, e);
    else hfs_delete(v, e);
}

void vol_mkdir(Volume *v, uint32_t dir, const MacName *name) {
    if (v->kind == FS_MFS) mfs_mkdir(v, dir, name);
    else hfs_mkdir(v, dir, name);
}

void vol_bless(Volume *v, uint32_t dir) {
    if (v->kind == FS_HFS) hfs_bless(v, dir);
}

void vol_startup_dir(Volume *v, uint32_t dir) {
    if (v->kind == FS_HFS) hfs_startup_dir(v, dir);
}

void vol_info(Volume *v, VolInfo *vi) {
    memset(vi, 0, sizeof *vi);
    vi->kind = v->kind;
    vi->boot_blocks = get16(v->img) == 0x4C4B;
    if (v->kind == FS_MFS) mfs_info(v, vi);
    else hfs_info(v, vi);
}
