/* btree.c - the B*-trees of HFS (catalog and extents overflow files).
 *
 * A B-tree file is an array of 512-byte nodes. Every node starts with a
 * 14-byte descriptor: 0 ndFLink, 4 ndBLink (next/previous node of the same
 * level), 8 ndType (0 index, 1 header, 2 map, $FF leaf), 9 ndNHeight (1 for
 * leaves), 10 ndNRecs. The offsets of its records are stored backwards from
 * the end of the node, followed by the offset of the free space.
 *
 * Node 0 is the header node with three records: the header record (0 depth,
 * 2 root, 6 number of leaf records, 10 first leaf, 14 last leaf, 18 node
 * size, 20 maximum key length, 22 number of nodes, 26 free nodes, then 76
 * reserved bytes), a 128-byte user data record and the node allocation map
 * (256 bytes, one bit per node, most significant bit first). Trees with more
 * than 2048 nodes continue the map in map nodes chained from the header.
 *
 * Records are a key (length byte first, padded to an even length) followed
 * by data. Index records point to the node below them with a 4-byte node
 * number; their key is the first key found in that node, padded to the
 * maximum key length. */
#include "btree.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>

#define DESC 14
#define HDR_MAP_BITS 2048           /* 256 bytes of map in the header node */
#define MAP_NODE_BITS 3952          /* 494 bytes of map in a map node */
#define MAX_LEVELS 16

int bt_read(const unsigned char *file, uint32_t nnodes, BtRec **recs, int *nrecs, const char **err) {
    *recs = NULL;
    *nrecs = 0;
    if (nnodes < 1 || file[8] != 1 || get16(file + BT_NODE - 2) != DESC) { *err = "B-tree header node missing"; return -1; }
    const unsigned char *hr = file + DESC;
    uint32_t fnode = get32(hr + 10);
    if (get16(hr + 18) != BT_NODE) { *err = "unsupported B-tree node size"; return -1; }
    if (get16(hr + 0) == 0 || fnode == 0) return 0;         /* empty tree */

    int cap = 64, n = 0;
    BtRec *r = xmalloc((size_t)cap * sizeof *r);
    uint32_t cur = fnode, seen = 0;
    while (cur != 0) {
        if (cur >= nnodes || ++seen > nnodes) { *err = "broken leaf node chain"; free(r); return -1; }
        const unsigned char *node = file + (size_t)cur * BT_NODE;
        if (node[8] != 0xFF) { *err = "leaf node chain leads to a node that is not a leaf"; free(r); return -1; }
        int k = (int)get16(node + 10);
        if (2 * (k + 1) > BT_NODE - DESC) { *err = "bad record count in a B-tree node"; free(r); return -1; }
        int limit = BT_NODE - 2 * (k + 1);
        for (int i = 0; i < k; i++) {
            int o = (int)get16(node + BT_NODE - 2 - 2 * i), e = (int)get16(node + BT_NODE - 4 - 2 * i);
            if (o < DESC || e <= o || e > limit) { *err = "bad record offsets in a B-tree node"; free(r); return -1; }
            int klen = node[o] + 1, kpad = klen + (klen & 1);
            if (o + kpad > e) { *err = "bad key length in a B-tree node"; free(r); return -1; }
            if (n == cap) { cap *= 2; r = xrealloc(r, (size_t)cap * sizeof *r); }
            r[n].key = node + o;
            r[n].klen = klen;
            r[n].data = node + o + kpad;
            r[n].dlen = e - o - kpad;
            n++;
        }
        cur = get32(node);
    }
    *recs = r;
    *nrecs = n;
    return 0;
}

static uint32_t map_nodes(uint32_t nnodes) {
    return nnodes > HDR_MAP_BITS ? (nnodes - HDR_MAP_BITS + MAP_NODE_BITS - 1) / MAP_NODE_BITS : 0;
}

static int leaf_size(const BtRec *r) { return r->klen + (r->klen & 1) + r->dlen; }

/* Packs records into nodes in order; starts[j] = first record of node j.
 * size == NULL: every record has the size 'fixed'. Returns the node count. */
static int pack(const BtRec *recs, int n, int fixed, int *starts) {
    int nodes = 0, i = 0;
    while (i < n) {
        int used = DESC + 2;
        starts[nodes++] = i;
        while (i < n) {
            int s = recs ? leaf_size(&recs[i]) : fixed;
            if (used + s + 2 > BT_NODE) break;
            used += s + 2;
            i++;
        }
        if (starts[nodes - 1] == i) fail(NULL, "internal error: B-tree record too large");
    }
    return nodes;
}

typedef struct {
    int levels;
    int count[MAX_LEVELS];          /* nodes per level, 0 = leaves */
    int *starts[MAX_LEVELS];        /* first record (child) of each node */
    uint32_t first[MAX_LEVELS];     /* node number of the first node of a level */
    uint32_t used;                  /* nodes in use, header and map nodes included */
    uint32_t mapfirst, nmap;
} Layout;

static void layout(Layout *L, const BtRec *recs, int n, int max_key_len, uint32_t nnodes) {
    memset(L, 0, sizeof *L);
    int idx_size = max_key_len + 1 + (max_key_len + 1) % 2 + 4;
    if (n > 0) {
        L->starts[0] = xmalloc((size_t)n * sizeof(int));
        L->count[0] = pack(recs, n, 0, L->starts[0]);
        L->levels = 1;
        while (L->count[L->levels - 1] > 1) {
            if (L->levels == MAX_LEVELS) fail(NULL, "internal error: B-tree too deep");
            int below = L->count[L->levels - 1];
            L->starts[L->levels] = xmalloc((size_t)below * sizeof(int));
            L->count[L->levels] = pack(NULL, below, idx_size, L->starts[L->levels]);
            L->levels++;
        }
    }
    uint32_t next = 1;
    for (int l = 0; l < L->levels; l++) { L->first[l] = next; next += (uint32_t)L->count[l]; }
    L->nmap = map_nodes(nnodes);
    L->mapfirst = next;
    L->used = next + L->nmap;
}

static void layout_free(Layout *L) {
    for (int l = 0; l < L->levels; l++) free(L->starts[l]);
}

uint32_t bt_nodes_needed(const BtRec *recs, int n, int max_key_len, uint32_t nnodes) {
    Layout L;
    layout(&L, recs, n, max_key_len, nnodes);
    layout_free(&L);
    return L.used;
}

static void set_offsets(unsigned char *node, const int *offs, int k) {
    for (int i = 0; i <= k; i++) put16(node + BT_NODE - 2 - 2 * i, (unsigned)offs[i]);
}

static void descriptor(unsigned char *node, uint32_t flink, uint32_t blink, int type, int height, int nrecs) {
    put32(node + 0, flink);
    put32(node + 4, blink);
    node[8] = (unsigned char)type;
    node[9] = (unsigned char)height;
    put16(node + 10, (unsigned)nrecs);
}

int bt_write(unsigned char *file, uint32_t nnodes, const BtRec *recs, int n, int max_key_len,
             const unsigned char *old_header) {
    Layout L;
    layout(&L, recs, n, max_key_len, nnodes);
    if (L.used > nnodes) { layout_free(&L); return -1; }
    memset(file, 0, (size_t)nnodes * BT_NODE);
    int keybytes = max_key_len + 1 + (max_key_len + 1) % 2;
    int offs[BT_NODE / 2];

    /* firstleaf[l][j]: index of the first leaf record below node j of level l */
    int *firstleaf[MAX_LEVELS] = {0};
    for (int l = 0; l < L.levels; l++) {
        firstleaf[l] = xmalloc((size_t)L.count[l] * sizeof(int));
        for (int j = 0; j < L.count[l]; j++)
            firstleaf[l][j] = l == 0 ? L.starts[0][j] : firstleaf[l - 1][L.starts[l][j]];
    }

    for (int l = 0; l < L.levels; l++) {
        int items = l == 0 ? n : L.count[l - 1];
        for (int j = 0; j < L.count[l]; j++) {
            uint32_t no = L.first[l] + (uint32_t)j;
            unsigned char *node = file + (size_t)no * BT_NODE;
            int a = L.starts[l][j], b = j + 1 < L.count[l] ? L.starts[l][j + 1] : items;
            descriptor(node, j + 1 < L.count[l] ? no + 1 : 0, j > 0 ? no - 1 : 0, l == 0 ? 0xFF : 0, l + 1, b - a);
            int o = DESC, k = 0;
            for (int i = a; i < b; i++) {
                offs[k++] = o;
                if (l == 0) {
                    const BtRec *r = &recs[i];
                    memcpy(node + o, r->key, (size_t)r->klen);
                    o += r->klen + (r->klen & 1);
                    memcpy(node + o, r->data, (size_t)r->dlen);
                    o += r->dlen;
                } else {
                    const BtRec *r = &recs[firstleaf[l - 1][i]];
                    int kl = r->klen > keybytes ? keybytes : r->klen;
                    memcpy(node + o, r->key, (size_t)kl);
                    node[o] = (unsigned char)max_key_len;
                    o += keybytes;
                    put32(node + o, L.first[l - 1] + (uint32_t)i);
                    o += 4;
                }
            }
            offs[k] = o;
            set_offsets(node, offs, k);
        }
    }

    /* header node */
    unsigned char *h = file;
    descriptor(h, L.nmap ? L.mapfirst : 0, 0, 1, 0, 3);
    unsigned char *hr = h + DESC;
    put16(hr + 0, (unsigned)L.levels);
    put32(hr + 2, L.levels ? L.first[L.levels - 1] : 0);
    put32(hr + 6, (uint32_t)n);
    put32(hr + 10, n ? L.first[0] : 0);
    put32(hr + 14, n ? L.first[0] + (uint32_t)L.count[0] - 1 : 0);
    put16(hr + 18, BT_NODE);
    put16(hr + 20, (unsigned)max_key_len);
    put32(hr + 22, nnodes);
    put32(hr + 26, nnodes - L.used);
    if (old_header) {
        memcpy(hr + 30, old_header + DESC + 30, 76);        /* reserved */
        memcpy(h + 120, old_header + 120, 128);             /* user data record */
    }
    int hoffs[4] = {DESC, 120, 248, 504};
    set_offsets(h, hoffs, 3);

    /* map nodes */
    for (uint32_t k = 0; k < L.nmap; k++) {
        uint32_t no = L.mapfirst + k;
        unsigned char *node = file + (size_t)no * BT_NODE;
        descriptor(node, k + 1 < L.nmap ? no + 1 : 0, 0, 2, 0, 1);
        int moffs[2] = {DESC, BT_NODE - 4};
        set_offsets(node, moffs, 1);
    }
    /* allocation map: nodes 0 .. used-1 are in use */
    for (uint32_t i = 0; i < L.used; i++) {
        unsigned char *bits;
        uint32_t bit;
        if (i < HDR_MAP_BITS) { bits = h + 248; bit = i; }
        else {
            uint32_t k = (i - HDR_MAP_BITS) / MAP_NODE_BITS;
            bits = file + (size_t)(L.mapfirst + k) * BT_NODE + DESC;
            bit = (i - HDR_MAP_BITS) % MAP_NODE_BITS;
        }
        bits[bit / 8] |= (unsigned char)(0x80 >> (bit % 8));
    }

    for (int l = 0; l < L.levels; l++) free(firstleaf[l]);
    layout_free(&L);
    return 0;
}
