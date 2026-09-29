/* vol.h - a Mac volume in a disk image: common interface of MFS and HFS. */
#ifndef MAC68K_DISK_VOL_H
#define MAC68K_DISK_VOL_H
#include "macbin.h"
#include "names.h"
#include <stddef.h>
#include <stdint.h>

typedef enum { FS_MFS = 1, FS_HFS = 2 } FsKind;

#define ROOT_ID 2           /* the root folder (HFS directory ID 2; the disk's window on MFS) */

/* A file or folder as the commands see it. */
typedef struct {
    int is_dir;
    MacName name;
    uint32_t id;            /* HFS catalog node ID, MFS file number */
    uint32_t parent;        /* folder it is in */
    unsigned char finfo[16];/* FInfo of a file, DInfo of a folder */
    uint32_t crdate, mddate;
    uint32_t dlen, rlen;    /* logical fork lengths */
    uint32_t dphys, rphys;  /* allocated bytes */
    unsigned valence;       /* items in a folder */
} Entry;

typedef struct {
    FsKind kind;
    MacName name;
    uint32_t crdate, mddate;        /* mddate 0 on MFS (not recorded) */
    uint32_t blksize, nblocks, freeblocks;
    unsigned files, folders;        /* whole volume, the root folder not counted */
    int boot_blocks;                /* boot blocks present ("LK") */
    MacName blessed;                /* HFS: the blessed System Folder; MFS: the folder with the System */
    unsigned dir_used, dir_size;    /* MFS: bytes of the file directory */
} VolInfo;

typedef struct Volume {
    const char *path;       /* image file, as given on the command line */
    unsigned char *img;     /* the whole image in memory */
    size_t size;
    FsKind kind;
    void *fs;               /* Mfs or Hfs */
} Volume;

/* Opens an image (whole file in memory). Exits with a message on anything it
 * cannot handle: unknown format, DiskCopy header, damaged structures. */
Volume *vol_open(const char *path);
/* Writes all changes back: file system structures, then the image file,
 * atomically. */
void vol_save(Volume *v);
/* Creates a new image file with an empty volume. */
void vol_create(const char *path, FsKind kind, size_t size, const MacName *name);
/* An empty volume in memory only; vol_save writes it to path. */
Volume *vol_new(const char *path, FsKind kind, size_t size, const MacName *name);

int  vol_max_name(const Volume *v);                                  /* 31 (HFS) or 63 (MFS) */
int  vol_list(Volume *v, uint32_t dir, Entry **out);                 /* sorted by name */
int  vol_find(Volume *v, uint32_t dir, const MacName *name, Entry *e);   /* 1 = found */
/* Like vol_find, but on MFS - where names are unique on the whole volume - a
 * file of that name in any folder counts. */
int  vol_find_name(Volume *v, uint32_t dir, const MacName *name, Entry *e);
void vol_read_fork(Volume *v, const Entry *e, int rsrc, unsigned char **data, uint32_t *len);
void vol_add_file(Volume *v, uint32_t dir, const MacFile *f);
void vol_delete(Volume *v, const Entry *e);
void vol_mkdir(Volume *v, uint32_t dir, const MacName *name);
/* HFS: makes dir the System Folder the Mac starts from (MFS: nothing to do,
 * the System is found anywhere). */
void vol_bless(Volume *v, uint32_t dir);
void vol_info(Volume *v, VolInfo *vi);

/* ---- file system back ends ---- */
void *mfs_open(Volume *v);
void  mfs_commit(Volume *v);
void  mfs_format(unsigned char *img, size_t size, const MacName *name);
int   mfs_list(Volume *v, uint32_t dir, Entry **out);
int   mfs_find_file(Volume *v, const MacName *name, Entry *e);
void  mfs_read_fork(Volume *v, const Entry *e, int rsrc, unsigned char **data, uint32_t *len);
void  mfs_add_file(Volume *v, uint32_t dir, const MacFile *f);
void  mfs_delete(Volume *v, const Entry *e);
void  mfs_mkdir(Volume *v, uint32_t dir, const MacName *name);
void  mfs_info(Volume *v, VolInfo *vi);

void *hfs_open(Volume *v);
void  hfs_commit(Volume *v);
void  hfs_format(unsigned char *img, size_t size, const MacName *name);
int   hfs_list(Volume *v, uint32_t dir, Entry **out);
void  hfs_read_fork(Volume *v, const Entry *e, int rsrc, unsigned char **data, uint32_t *len);
void  hfs_add_file(Volume *v, uint32_t dir, const MacFile *f);
void  hfs_delete(Volume *v, const Entry *e);
void  hfs_mkdir(Volume *v, uint32_t dir, const MacName *name);
void  hfs_bless(Volume *v, uint32_t dir);
void  hfs_info(Volume *v, VolInfo *vi);

/* Size in KB, rounded up, for messages. */
unsigned kb(uint64_t bytes);

#endif
