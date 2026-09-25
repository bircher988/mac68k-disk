/* btree.h - the B*-trees of HFS (catalog and extents overflow files). */
#ifndef MAC68K_DISK_BTREE_H
#define MAC68K_DISK_BTREE_H
#include <stddef.h>
#include <stdint.h>

#define BT_NODE 512

/* A leaf record. key points at the key length byte; klen counts all key
 * bytes including that byte (unpadded); data follows the word-aligned key. */
typedef struct {
    const unsigned char *key;
    int klen;
    const unsigned char *data;
    int dlen;
} BtRec;

/* Reads all leaf records, in order, from a B-tree file of nnodes nodes.
 * Returns 0, or -1 with a reason in err. */
int bt_read(const unsigned char *file, uint32_t nnodes, BtRec **recs, int *nrecs, const char **err);

/* Nodes a tree with these (sorted) records needs, header and map nodes
 * included, if the file has nnodes nodes. */
uint32_t bt_nodes_needed(const BtRec *recs, int n, int max_key_len, uint32_t nnodes);

/* Writes a complete, freshly packed tree over file (nnodes nodes). Leaf nodes
 * are filled in order, index nodes above them carry keys padded to
 * max_key_len (37 for the catalog, 7 for the extents file). The reserved
 * part of the header record and the user data record are taken from
 * old_header (a header node) when it is given. Returns 0, or -1 if the
 * records do not fit into nnodes nodes. */
int bt_write(unsigned char *file, uint32_t nnodes, const BtRec *recs, int n, int max_key_len,
             const unsigned char *old_header);

#endif
