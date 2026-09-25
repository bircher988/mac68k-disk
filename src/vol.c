/* vol.c - opening, creating and saving disk images; dispatch to MFS or HFS. */
#include "vol.h"
#include "util.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define BLK 512

unsigned kb(uint64_t bytes) { return (unsigned)((bytes + 1023) / 1024); }

/* DiskCopy 4.2: an 84-byte header (name, data size at 64, tag size at 68,
 * checksums, format bytes, $0100 at 82) in front of the sectors. */
static int is_diskcopy42(const unsigned char *d, size_t n) {
    if (n < 84 || d[82] != 0x01 || d[83] != 0x00 || d[0] == 0 || d[0] > 63) return 0;
    uint64_t data = get32(d + 64), tags = get32(d + 68);
    return data % BLK == 0 && 84 + data + tags == n;
}

Volume *vol_open(const char *path) {
    Volume *v = xcalloc(1, sizeof *v);
    v->path = path;
    if (read_file(path, &v->img, &v->size)) {
        if (errno == ENOENT) fail(path, "no such file (create a disk with: mac68k-disk new %s)", path);
        fail(path, "cannot read: %s", strerror(errno));
    }
    if (is_diskcopy42(v->img, v->size))
        fail(path, "DiskCopy 4.2 images are not supported yet (only raw sector images: .dsk, .img)");
    if (v->size < 3 * BLK) fail(path, "too small for a Mac disk image");
    if (v->size % BLK) fail(path, "not a raw disk image (size is not a multiple of 512 bytes)");
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
    if (write_file_atomic(v->path, v->img, v->size)) fail(v->path, "cannot write: %s", strerror(errno));
}

void vol_create(const char *path, FsKind kind, size_t size, const MacName *name) {
    unsigned char *img = xcalloc(size, 1);
    if (kind == FS_MFS) mfs_format(img, size, name);
    else hfs_format(img, size, name);
    if (write_file_atomic(path, img, size)) fail(path, "cannot write: %s", strerror(errno));
    free(img);
}

int vol_max_name(const Volume *v) { return v->kind == FS_MFS ? 63 : 31; }

static int entry_cmp(const void *a, const void *b) {
    const Entry *x = a, *y = b;
    int c = name_cmp(x->name.s, x->name.len, y->name.s, y->name.len);
    if (c) return c;
    return memcmp(x->name.s, y->name.s, x->name.len < y->name.len ? x->name.len : y->name.len);
}

int vol_list(Volume *v, uint32_t dir, Entry **out) {
    int n = v->kind == FS_MFS ? mfs_list(v, out) : hfs_list(v, dir, out);
    qsort(*out, (size_t)n, sizeof **out, entry_cmp);
    return n;
}

int vol_find(Volume *v, uint32_t dir, const MacName *name, Entry *e) {
    Entry *list;
    int n = v->kind == FS_MFS ? mfs_list(v, &list) : hfs_list(v, dir, &list), found = 0;
    for (int i = 0; i < n && !found; i++) {
        if (name_cmp(list[i].name.s, list[i].name.len, name->s, name->len) == 0) { *e = list[i]; found = 1; }
    }
    free(list);
    return found;
}

void vol_read_fork(Volume *v, const Entry *e, int rsrc, unsigned char **data, uint32_t *len) {
    if (v->kind == FS_MFS) mfs_read_fork(v, e, rsrc, data, len);
    else hfs_read_fork(v, e, rsrc, data, len);
}

void vol_add_file(Volume *v, uint32_t dir, const MacFile *f) {
    if (v->kind == FS_MFS) mfs_add_file(v, f);
    else hfs_add_file(v, dir, f);
}

void vol_delete(Volume *v, const Entry *e) {
    if (v->kind == FS_MFS) mfs_delete(v, e);
    else hfs_delete(v, e);
}

void vol_mkdir(Volume *v, uint32_t dir, const MacName *name) {
    if (v->kind == FS_MFS) fail(v->path, "MFS volumes have no folders (the file system is flat)");
    hfs_mkdir(v, dir, name);
}

void vol_info(Volume *v, VolInfo *vi) {
    memset(vi, 0, sizeof *vi);
    vi->kind = v->kind;
    vi->boot_blocks = get16(v->img) == 0x4C4B;
    if (v->kind == FS_MFS) mfs_info(v, vi);
    else hfs_info(v, vi);
}
