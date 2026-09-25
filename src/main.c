/* main.c - mac68k-disk command line: new, add, ls, get, rm, mkdir, info, startup, version, help. */
#define _POSIX_C_SOURCE 200809L
#include "macbin.h"
#include "names.h"
#include "util.h"
#include "vol.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define MAC68K_DISK_VERSION "1.0"

/* Finder flags that describe a file's state on the disk it came from: on the
 * desktop, icon placed (inited), changed, busy. New files start without them
 * so the Finder places the icon itself. */
#define FLAGS_TO_CLEAR (0x0001 | 0x0100 | 0x0200 | 0x0400)

static void usage(FILE *f) {
    fputs("usage: mac68k-disk new   <image> [--hfs] [--size 400k|800k|1440k] [--name <volume name>]\n"
          "       mac68k-disk add   <image> <file>... [--name <mac name>] [--type TTTT --creator CCCC]\n"
          "                         [--rsrc <file>] [-f]\n"
          "       mac68k-disk ls    <image> [-l] [-R] [<folder>]\n"
          "       mac68k-disk get   <image> <mac name> [-o <out>] [--data|--rsrc]\n"
          "       mac68k-disk rm    <image> <mac name>\n"
          "       mac68k-disk mkdir <image> <folder>\n"
          "       mac68k-disk info  <image>\n"
          "       mac68k-disk startup <image> [<application>] [-f]\n"
          "       mac68k-disk version\n"
          "       mac68k-disk help\n"
          "\n"
          "  new    create an empty disk image: MFS 400K (readable by every 68k Mac) or,\n"
          "         with --hfs, HFS (800k unless --size says otherwise); the volume name\n"
          "         defaults to the image file name without its extension\n"
          "  add    copy files onto the disk; a <file> is MacBinary (I, II or III) or a\n"
          "         raw data fork (type and creator ???? unless --type/--creator);\n"
          "         --name renames (single file) or, ending in ':', picks the folder;\n"
          "         --rsrc adds a resource fork; -f replaces a file of the same name\n"
          "  ls     list a folder (-l: type, creator, data/rsrc sizes, date; -R: all folders)\n"
          "  get    copy a file to <name>.bin (MacBinary II) or, with --data/--rsrc,\n"
          "         one fork as it is; -o - writes to standard output\n"
          "  rm     delete a file or an empty folder\n"
          "  mkdir  create a folder (HFS only)\n"
          "  info   format, volume name, sizes, free space, dates\n"
          "  startup  show or set the application a startup disk opens when it boots\n"
          "         (Finder = the normal desktop); -f sets a name not found on the disk\n"
          "\n"
          "Images are raw sector images (.dsk, .img) as used by Mini vMac, Basilisk II\n"
          "and floppy emulators. On HFS, ':' separates folders: \"Games:Hello\".\n", f);
}

static int bad_usage(void) { usage(stderr); return 2; }

/* ---- arguments ---- */

typedef struct { const char **v; int n; } Args;

static void args_add(Args *a, const char *s) {
    a->v = xrealloc(a->v, (size_t)(a->n + 1) * sizeof *a->v);
    a->v[a->n++] = s;
}

/* Matches "--opt value" and "--opt=value". */
static int opt_value(int argc, char **argv, int *i, const char *opt, const char **val) {
    size_t n = strlen(opt);
    if (strncmp(argv[*i], opt, n)) return 0;
    if (argv[*i][n] == '=') { *val = argv[*i] + n + 1; return 1; }
    if (argv[*i][n]) return 0;
    if (*i + 1 >= argc) fail(prog_name, "%s needs a value", opt);
    *val = argv[++*i];
    return 1;
}

static void unknown_option(const char *o) {
    fprintf(stderr, "%s: unknown option '%s'\n", prog_name, o);
    exit(bad_usage());
}

/* ---- names and paths ---- */

static MacName mac_name(const char *what, const char *s, size_t n, int max, const char *hint) {
    MacName m;
    const char *err = name_from_utf8(s, n, &m) ? "too long" : name_check(&m, max);
    if (err) fail(what, "bad Mac name \"%.*s\": %s%s", (int)n, s, err, hint ? hint : "");
    return m;
}

/* Resolves the folder part of a Mac path ("Games:Hello", ":Games:Hello") and
 * returns the last component in leaf (empty for "" or a path ending in ':'). */
static uint32_t resolve_parent(Volume *v, const char *path, MacName *leaf) {
    const char *p = path;
    if (*p == ':') p++;
    uint32_t dir = ROOT_ID;
    for (;;) {
        const char *c = strchr(p, ':');
        if (!c) break;
        if (v->kind == FS_MFS) fail(v->path, "MFS volumes have no folders (the file system is flat): \"%s\"", path);
        MacName n = mac_name(v->path, p, (size_t)(c - p), 31, NULL);
        Entry e;
        if (!vol_find(v, dir, &n, &e)) fail(v->path, "no folder \"%.*s\"", (int)(c - path), path);
        if (!e.is_dir) fail(v->path, "\"%.*s\" is a file, not a folder", (int)(c - path), path);
        dir = e.id;
        p = c + 1;
    }
    leaf->len = 0;
    if (*p) *leaf = mac_name(v->path, p, strlen(p), vol_max_name(v), NULL);
    return dir;
}

static char *strip_colon(const char *s) {
    char *c = xstrdup(s);
    size_t n = strlen(c);
    if (n > 1 && c[n - 1] == ':') c[n - 1] = 0;
    return c;
}

/* Finds the file or folder a path names; the root folder for "" and ":". */
static int lookup(Volume *v, const char *path, Entry *e) {
    char *p = strip_colon(path);
    MacName leaf;
    uint32_t dir = resolve_parent(v, p, &leaf);
    free(p);
    if (leaf.len == 0) {
        memset(e, 0, sizeof *e);
        e->is_dir = 1;
        e->id = ROOT_ID;
        return 1;
    }
    return vol_find(v, dir, &leaf, e);
}

static void fourcc(const unsigned char c[4], char out[16]) { ostype_str(c, out); }

/* ---- new ---- */

static size_t parse_size(const char *s) {
    char *end;
    double n = strtod(s, &end);
    if (end == s) return 0;
    if (!strcasecmp(end, "m") || !strcasecmp(end, "mb")) n *= 1024;
    else if (*end && strcasecmp(end, "k") && strcasecmp(end, "kb")) return 0;
    if (n > 1400 && n < 1500) n = 1440;
    return (size_t)n * 1024;
}

static int cmd_new(int argc, char **argv) {
    Args pos = {0};
    int hfs = 0;
    const char *size_s = NULL, *name_s = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--hfs")) hfs = 1;
        else if (opt_value(argc, argv, &i, "--size", &size_s) || opt_value(argc, argv, &i, "--name", &name_s)) {}
        else if (argv[i][0] == '-' && argv[i][1]) unknown_option(argv[i]);
        else args_add(&pos, argv[i]);
    }
    if (pos.n != 1) return bad_usage();
    const char *path = pos.v[0];
    size_t size = size_s ? parse_size(size_s) : hfs ? 800 * 1024 : 400 * 1024;
    if (size != 400 * 1024 && size != 800 * 1024 && size != 1440 * 1024)
        fail(prog_name, "unsupported size '%s' (use 400k, 800k or 1440k)", size_s);
    if (!hfs && size != 400 * 1024) fail(prog_name, "MFS disks are 400k; use --hfs for %uk", (unsigned)(size / 1024));
    MacName name;
    if (name_s) name = mac_name(path, name_s, strlen(name_s), 27, NULL);
    else {
        const char *base = path_basename(path), *dot = strrchr(base, '.');
        size_t n = dot && dot != base ? (size_t)(dot - base) : strlen(base);
        if (name_from_utf8(base, n, &name) || name.len == 0) fail(path, "cannot derive a volume name, use --name");
        if (name.len > 27) name.len = 27;
        for (int i = 0; i < name.len; i++) if (name.s[i] == ':' || name.s[i] < 0x20) name.s[i] = '_';
    }
    if (path_exists(path)) fail(path, "already exists (delete it first to start over)");
    vol_create(path, hfs ? FS_HFS : FS_MFS, size, &name);
    Volume *v = vol_open(path);
    VolInfo vi;
    vol_info(v, &vi);
    printf("%s: new %uK %s disk \"%s\", %u KB free\n", path, (unsigned)(size / 1024), hfs ? "HFS" : "MFS",
           name_str(&name), (unsigned)((uint64_t)vi.freeblocks * vi.blksize / 1024));
    return 0;
}

/* ---- add ---- */

static unsigned char *read_input(const char *path, size_t *len) {
    struct stat st;
    unsigned char *d;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) fail(path, "is a directory");
    if (read_file(path, &d, len)) fail(path, "cannot read: %s", strerror(errno));
    if (*len > 0x7FFFFFFF) fail(path, "too large");
    return d;
}

static int ends_with_ci(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n > m && !strcasecmp(s + n - m, suffix);
}

static int cmd_add(int argc, char **argv) {
    Args pos = {0};
    const char *name_s = NULL, *type_s = NULL, *creator_s = NULL, *rsrc_s = NULL;
    int force = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "--force")) force = 1;
        else if (opt_value(argc, argv, &i, "--name", &name_s) || opt_value(argc, argv, &i, "--type", &type_s)
                 || opt_value(argc, argv, &i, "--creator", &creator_s) || opt_value(argc, argv, &i, "--rsrc", &rsrc_s)) {}
        else if (argv[i][0] == '-' && argv[i][1]) unknown_option(argv[i]);
        else args_add(&pos, argv[i]);
    }
    if (pos.n < 1 || (pos.n < 2 && !rsrc_s)) return bad_usage();
    int nfiles = pos.n - 1;
    int name_is_folder = name_s && *name_s && name_s[strlen(name_s) - 1] == ':';
    if (name_s && nfiles > 1 && !name_is_folder) fail(prog_name, "--name works with a single file only (or names a folder when it ends in ':')");
    if (rsrc_s && nfiles > 1) fail(prog_name, "--rsrc works with a single file only");
    if (nfiles == 0 && (!name_s || name_is_folder)) fail(prog_name, "a file made from --rsrc alone needs --name");
    unsigned char type[4], creator[4];
    if (type_s && ostype_parse(type_s, type)) fail(prog_name, "--type needs exactly 4 characters, e.g. APPL or TEXT");
    if (creator_s && ostype_parse(creator_s, creator)) fail(prog_name, "--creator needs exactly 4 characters, e.g. ttxt");

    Volume *v = vol_open(pos.v[0]);
    uint32_t now = mac_now();
    for (int k = 0; k < (nfiles ? nfiles : 1); k++) {
        const char *src = nfiles ? pos.v[1 + k] : NULL;
        MacFile f;
        memset(&f, 0, sizeof f);
        unsigned char *buf = NULL, *rbuf = NULL;
        size_t len = 0, rlen = 0;
        int macbin = 0;
        if (src) {
            buf = read_input(src, &len);
            macbin = macbin_detect(buf, len);
            if (macbin) {
                if (macbin_parse(buf, len, &f)) fail(src, "damaged MacBinary file");
            } else {
                if (ends_with_ci(src, ".bin")) warn(src, "not a valid MacBinary file, adding it as a raw data fork");
                const char *base = path_basename(src);
                size_t n = strlen(base);
                if (ends_with_ci(base, ".bin")) n -= 4;
                if (name_from_utf8(base, n, &f.name)) fail(src, "file name too long for a Mac name, use --name");
                memcpy(f.type, "????", 4);
                memcpy(f.creator, "????", 4);
                f.data = buf;
                f.dlen = (uint32_t)len;
            }
        } else {
            memcpy(f.type, "????", 4);
            memcpy(f.creator, "????", 4);
        }
        if (rsrc_s) {
            rbuf = read_input(rsrc_s, &rlen);
            f.rsrc = rbuf;
            f.rlen = (uint32_t)rlen;
        }
        if (type_s) memcpy(f.type, type, 4);
        if (creator_s) memcpy(f.creator, creator, 4);
        f.flags &= ~(unsigned)FLAGS_TO_CLEAR;
        f.v = f.h = f.fldr = 0;
        if (!f.crdate) f.crdate = now;
        if (!f.mddate) f.mddate = now;

        /* where it goes and under which name */
        uint32_t dir = ROOT_ID;
        const char *hint = name_s ? NULL : " (choose another with --name)";
        if (name_s) {
            MacName leaf;
            dir = resolve_parent(v, name_s, &leaf);
            if (leaf.len) f.name = leaf;
        }
        const char *err = name_check(&f.name, vol_max_name(v));
        if (err) fail(v->path, "bad Mac name \"%s\": %s%s", name_str(&f.name), err, hint ? hint : "");

        Entry old;
        int replaced = 0;
        if (vol_find(v, dir, &f.name, &old)) {
            if (old.is_dir) fail(v->path, "\"%s\" is a folder", name_str(&f.name));
            if (!force) fail(v->path, "\"%s\" already exists (use -f to replace it)", name_str(&f.name));
            vol_delete(v, &old);
            replaced = 1;
        }
        vol_add_file(v, dir, &f);
        char t[16], c[16];
        fourcc(f.type, t);
        fourcc(f.creator, c);
        printf("%s %s (%s/%s, %s%u bytes)\n", replaced ? "replaced" : "added", name_str(&f.name), t, c,
               macbin ? "MacBinary, " : "", f.dlen + f.rlen);
        free(buf);
        free(rbuf);
    }
    vol_save(v);
    VolInfo vi;
    vol_info(v, &vi);
    printf("%s: %u KB free\n", v->path, (unsigned)((uint64_t)vi.freeblocks * vi.blksize / 1024));
    return 0;
}

/* ---- ls ---- */

static void print_entry(const Entry *e, const char *prefix, int longfmt) {
    char name[800], date[32];
    name_to_utf8(e->name.s, e->name.len, name);
    if (!longfmt) { printf("%s%s%s\n", prefix, name, e->is_dir ? ":" : ""); return; }
    mac_date_str(e->mddate, date);
    if (e->is_dir) {
        char items[24];
        snprintf(items, sizeof items, "%u item%s", e->valence, e->valence == 1 ? "" : "s");
        printf("%-9s %10s %10s  %-16s  %s%s:\n", "folder", items, "", date, prefix, name);
    } else {
        char t[16], c[16];
        fourcc(e->finfo, t);
        fourcc(e->finfo + 4, c);
        printf("%s %s %10u %10u  %-16s  %s%s\n", t, c, e->dlen, e->rlen, date, prefix, name);
    }
}

static void list_dir(Volume *v, uint32_t dir, const char *prefix, int longfmt, int recursive) {
    Entry *list;
    int n = vol_list(v, dir, &list);
    for (int i = 0; i < n; i++) {
        print_entry(&list[i], prefix, longfmt);
        if (recursive && list[i].is_dir) {
            char name[800];
            name_to_utf8(list[i].name.s, list[i].name.len, name);
            char *sub = xsprintf("%s%s:", prefix, name);
            list_dir(v, list[i].id, sub, longfmt, recursive);
            free(sub);
        }
    }
    free(list);
}

static int cmd_ls(int argc, char **argv) {
    Args pos = {0};
    int longfmt = 0, recursive = 0;
    for (int i = 2; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] && argv[i][1] != '-') {
            for (const char *p = argv[i] + 1; *p; p++) {
                if (*p == 'l') longfmt = 1;
                else if (*p == 'R') recursive = 1;
                else unknown_option(argv[i]);
            }
        } else if (argv[i][0] == '-' && argv[i][1]) unknown_option(argv[i]);
        else args_add(&pos, argv[i]);
    }
    if (pos.n < 1 || pos.n > 2) return bad_usage();
    Volume *v = vol_open(pos.v[0]);
    Entry e;
    const char *folder = pos.n == 2 ? pos.v[1] : "";
    if (!lookup(v, folder, &e)) fail(v->path, "no file or folder \"%s\"", folder);
    if (longfmt) printf("%-9s %10s %10s  %-16s  %s\n", "TYPE CREA", "DATA", "RSRC", "MODIFIED", "NAME");
    if (e.is_dir) list_dir(v, e.id, "", longfmt, recursive);
    else print_entry(&e, "", longfmt);
    return 0;
}

/* ---- get ---- */

static int cmd_get(int argc, char **argv) {
    Args pos = {0};
    const char *out = NULL;
    int data_only = 0, rsrc_only = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--data")) data_only = 1;
        else if (!strcmp(argv[i], "--rsrc")) rsrc_only = 1;
        else if (opt_value(argc, argv, &i, "-o", &out)) {}
        else if (argv[i][0] == '-' && argv[i][1]) unknown_option(argv[i]);
        else args_add(&pos, argv[i]);
    }
    if (pos.n != 2 || (data_only && rsrc_only)) return bad_usage();
    Volume *v = vol_open(pos.v[0]);
    Entry e;
    if (!lookup(v, pos.v[1], &e)) fail(v->path, "no file \"%s\"", pos.v[1]);
    if (e.is_dir) fail(v->path, "\"%s\" is a folder", pos.v[1]);

    MacFile f;
    memset(&f, 0, sizeof f);
    unsigned char *d = NULL, *r = NULL;
    if (!rsrc_only) vol_read_fork(v, &e, 0, &d, &f.dlen);
    if (!data_only) vol_read_fork(v, &e, 1, &r, &f.rlen);
    f.data = d;
    f.rsrc = r;
    f.name = e.name;
    memcpy(f.type, e.finfo, 4);
    memcpy(f.creator, e.finfo + 4, 4);
    f.flags = get16(e.finfo + 8);
    f.v = (int16_t)get16(e.finfo + 10);
    f.h = (int16_t)get16(e.finfo + 12);
    f.fldr = (int16_t)get16(e.finfo + 14);
    f.crdate = e.crdate;
    f.mddate = e.mddate;

    const unsigned char *bytes;
    unsigned char *mb = NULL;
    size_t len;
    if (data_only) { bytes = d; len = f.dlen; }
    else if (rsrc_only) { bytes = r; len = f.rlen; }
    else { mb = macbin_build(&f, &len); bytes = mb; }

    char *path;
    if (out) path = xstrdup(out);
    else {
        char name[800];
        name_to_utf8(e.name.s, e.name.len, name);
        for (char *p = name; *p; p++) if (*p == '/') *p = '-';
        path = xsprintf("%s%s", name, data_only ? "" : rsrc_only ? ".rsrc" : ".bin");
    }
    if (!strcmp(path, "-")) {
        if (fwrite(bytes, 1, len, stdout) != len || fflush(stdout)) fail(prog_name, "cannot write to standard output");
    } else {
        if (write_file(path, bytes, len)) fail(path, "cannot write: %s", strerror(errno));
        char t[16], c[16];
        fourcc(f.type, t);
        fourcc(f.creator, c);
        printf("%s -> %s (%s, %s/%s, %zu bytes)\n", name_str(&e.name), path,
               data_only ? "data fork" : rsrc_only ? "resource fork" : "MacBinary II", t, c, len);
    }
    free(path);
    free(mb);
    free(d);
    free(r);
    return 0;
}

/* ---- rm, mkdir ---- */

static int cmd_rm(int argc, char **argv) {
    Args pos = {0};
    for (int i = 2; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) unknown_option(argv[i]);
        else args_add(&pos, argv[i]);
    }
    if (pos.n != 2) return bad_usage();
    Volume *v = vol_open(pos.v[0]);
    Entry e;
    if (!lookup(v, pos.v[1], &e)) fail(v->path, "no file or folder \"%s\"", pos.v[1]);
    if (e.id == ROOT_ID && e.is_dir) fail(v->path, "cannot remove the root folder");
    vol_delete(v, &e);
    vol_save(v);
    printf("removed %s%s\n", name_str(&e.name), e.is_dir ? ":" : "");
    return 0;
}

static int cmd_mkdir(int argc, char **argv) {
    Args pos = {0};
    for (int i = 2; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) unknown_option(argv[i]);
        else args_add(&pos, argv[i]);
    }
    if (pos.n != 2) return bad_usage();
    Volume *v = vol_open(pos.v[0]);
    if (v->kind == FS_MFS) fail(v->path, "MFS volumes have no folders (the file system is flat); use an HFS disk");
    char *p = strip_colon(pos.v[1]);
    MacName leaf;
    uint32_t dir = resolve_parent(v, p, &leaf);
    if (leaf.len == 0) return bad_usage();
    Entry e;
    if (vol_find(v, dir, &leaf, &e)) fail(v->path, "\"%s\" already exists", p);
    vol_mkdir(v, dir, &leaf);
    vol_save(v);
    printf("created folder %s:\n", p);
    free(p);
    return 0;
}

/* ---- startup ---- */

/* Boot blocks: after the header come Str15 names, 16 bytes each. bbHelloName is
 * the application the system opens after booting - normally the Finder. */
#define BB_HELLO 0x5A

static void hello_name(const unsigned char *bb, char *out) {
    name_to_utf8(bb + BB_HELLO + 1, bb[BB_HELLO] > 15 ? 15 : bb[BB_HELLO], out);
}

/* Is there a file of that name where the system looks for it: the only folder
 * of an MFS disk, on HFS the blessed System Folder (or the root)? */
static int startup_file_exists(Volume *v, const MacName *n) {
    Entry e;
    if (vol_find(v, ROOT_ID, n, &e) && !e.is_dir) return 1;
    if (v->kind != FS_HFS) return 0;
    VolInfo vi;
    vol_info(v, &vi);
    Entry sf;
    if (!vi.blessed.len || !vol_find(v, ROOT_ID, &vi.blessed, &sf) || !sf.is_dir) return 0;
    return vol_find(v, sf.id, n, &e) && !e.is_dir;
}

static int cmd_startup(int argc, char **argv) {
    Args pos = {0};
    int force = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-f")) force = 1;
        else if (argv[i][0] == '-' && argv[i][1]) unknown_option(argv[i]);
        else args_add(&pos, argv[i]);
    }
    if (pos.n < 1 || pos.n > 2) return bad_usage();
    Volume *v = vol_open(pos.v[0]);
    if (get16(v->img) != 0x4C4B)
        fail(v->path, "no boot blocks - not a startup disk (start from a copy of a System disk)");
    char s[64];
    if (pos.n == 1) {
        hello_name(v->img, s);
        printf("%s: opens \"%s\" at startup\n", v->path, s);
        return 0;
    }
    MacName n = mac_name(v->path, pos.v[1], strlen(pos.v[1]), 15, " (the boot blocks have room for 15 characters)");
    if (!force && !startup_file_exists(v, &n))
        fail(v->path, "no file \"%s\" %s (-f sets the name anyway)", name_str(&n),
             v->kind == FS_HFS ? "in the System Folder" : "on the disk");
    memset(v->img + BB_HELLO, 0, 16);
    v->img[BB_HELLO] = n.len;
    memcpy(v->img + BB_HELLO + 1, n.s, n.len);
    vol_save(v);
    hello_name(v->img, s);
    printf("%s: opens \"%s\" at startup\n", v->path, s);
    return 0;
}

/* ---- info ---- */

static int cmd_info(int argc, char **argv) {
    Args pos = {0};
    for (int i = 2; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) unknown_option(argv[i]);
        else args_add(&pos, argv[i]);
    }
    if (pos.n != 1) return bad_usage();
    Volume *v = vol_open(pos.v[0]);
    VolInfo vi;
    vol_info(v, &vi);
    char date[32];
    uint64_t total = (uint64_t)vi.nblocks * vi.blksize, freeb = (uint64_t)vi.freeblocks * vi.blksize;
    printf("%s: %s volume \"%s\", %zuK image\n", v->path, vi.kind == FS_MFS ? "MFS" : "HFS", name_str(&vi.name), v->size / 1024);
    printf("  blocks    %u allocation blocks of %u bytes, %u free\n", vi.nblocks, vi.blksize, vi.freeblocks);
    printf("  space     %u KB free of %u KB\n", (unsigned)(freeb / 1024), (unsigned)(total / 1024));
    if (vi.kind == FS_MFS)
        printf("  files     %u (file directory %u of %u bytes used)\n", vi.files, vi.dir_used, vi.dir_size);
    else
        printf("  contents  %u file%s, %u folder%s\n", vi.files, vi.files == 1 ? "" : "s", vi.folders, vi.folders == 1 ? "" : "s");
    mac_date_str(vi.crdate, date);
    printf("  created   %s\n", date);
    if (vi.kind == FS_HFS) {
        mac_date_str(vi.mddate, date);
        printf("  modified  %s\n", date);
    }
    if (vi.blessed.len) printf("  startup   %s (System Folder \"%s\")\n", vi.boot_blocks ? "yes" : "no boot blocks", name_str(&vi.blessed));
    else printf("  startup   %s\n", vi.boot_blocks ? "yes (boot blocks present)" : "no");
    if (vi.boot_blocks) {
        char s[64];
        hello_name(v->img, s);
        printf("  opens     %s\n", s);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) return bad_usage();
    const char *cmd = argv[1];
    if (!strcmp(cmd, "version") || !strcmp(cmd, "--version") || !strcmp(cmd, "-V")) {
        printf("mac68k-disk %s\n", MAC68K_DISK_VERSION);
        return 0;
    }
    if (!strcmp(cmd, "help") || !strcmp(cmd, "--help") || !strcmp(cmd, "-h")) { usage(stdout); return 0; }
    if (!strcmp(cmd, "new")) return cmd_new(argc, argv);
    if (!strcmp(cmd, "add")) return cmd_add(argc, argv);
    if (!strcmp(cmd, "ls")) return cmd_ls(argc, argv);
    if (!strcmp(cmd, "get")) return cmd_get(argc, argv);
    if (!strcmp(cmd, "rm")) return cmd_rm(argc, argv);
    if (!strcmp(cmd, "mkdir")) return cmd_mkdir(argc, argv);
    if (!strcmp(cmd, "info")) return cmd_info(argc, argv);
    if (!strcmp(cmd, "startup")) return cmd_startup(argc, argv);
    fprintf(stderr, "%s: unknown command '%s'\n", prog_name, cmd);
    return bad_usage();
}
