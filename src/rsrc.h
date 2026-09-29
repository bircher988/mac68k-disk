/* rsrc.h - reading and writing resource forks (Inside Macintosh I-128). */
#ifndef MAC68K_DISK_RSRC_H
#define MAC68K_DISK_RSRC_H
#include "names.h"
#include <stdint.h>

typedef struct {
    unsigned char type[4];
    int id;                 /* -32768 .. 32767 */
    int has_name;
    MacName name;
    unsigned attrs;
    unsigned char *data;    /* malloc'd */
    uint32_t len;
} Res;

typedef struct {
    Res *v;
    int n;
    unsigned attrs;         /* resource map attributes */
} ResFile;

/* Parses a resource fork (copies everything). An empty fork gives an empty
 * file. Returns 0, or -1 if the fork is damaged. */
int rsrc_parse(const unsigned char *buf, uint32_t len, ResFile *rf);

/* Builds a resource fork: resources grouped by type in order of appearance,
 * data in the same order; the result is malloc'd. */
unsigned char *rsrc_build(const ResFile *rf, uint32_t *len);

Res *rsrc_get(ResFile *rf, const char *type, int id);
/* Adds a resource or replaces the one with the same type and ID; takes a copy
 * of data. A NULL name means no name. */
Res *rsrc_put(ResFile *rf, const char *type, int id, const MacName *name, const unsigned char *data, uint32_t len);
void rsrc_del(ResFile *rf, const char *type, int id);
void rsrc_free(ResFile *rf);

#endif
