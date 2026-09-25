/* macbin.h - MacBinary I/II/III reader and MacBinary II writer. */
#ifndef MAC68K_DISK_MACBIN_H
#define MAC68K_DISK_MACBIN_H
#include "names.h"
#include <stddef.h>
#include <stdint.h>

/* A Mac file outside of any volume: name, Finder info, dates, both forks. */
typedef struct {
    MacName name;
    unsigned char type[4], creator[4];
    unsigned flags;                 /* Finder flags (fdFlags) */
    int v, h;                       /* icon position (fdLocation) */
    int fldr;                       /* window / folder (fdFldr) */
    uint32_t crdate, mddate;        /* Mac dates, 0 = unknown */
    const unsigned char *data; uint32_t dlen;
    const unsigned char *rsrc; uint32_t rlen;
} MacFile;

/* Returns the MacBinary version of buf (1, 2 or 3) or 0 if it is not a
 * MacBinary file. Versions 2 and 3 are recognised by the header CRC, version
 * 1 by its zero fields and fork lengths that match the file size exactly. */
int macbin_detect(const unsigned char *buf, size_t len);

/* Fills f from a MacBinary file (pointers into buf). Returns 0 or -1. */
int macbin_parse(const unsigned char *buf, size_t len, MacFile *f);

/* Builds a MacBinary II file; the result is malloc'd. */
unsigned char *macbin_build(const MacFile *f, size_t *outlen);

uint16_t macbin_crc(const unsigned char *p, size_t n);   /* CRC-16/XMODEM */

#endif
