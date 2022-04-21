#ifndef _LUA_CUCKOOFILTER_H_
#define _LUA_CUCKOOFILTER_H_

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <lua.h>
#include <lauxlib.h>
#include "komihash/komihash.h"

#define MT_NAME ("_cuckoofilter_metatable")

// maximum number of cuckoo kicks before claiming failure
#define MAX_CUCKOO_COUNT 500
// all buckets = 4 half sort table situation
#define N_ENTS 3876

// packed table
#define FPSIZE 4
#define TAGS_PER_TABLE 4
#define CODE_SIZE 12

const size_t BITS_PER_BYTE = sizeof(char) * 8;
const size_t BYTES_PER_UINT64 = sizeof(uint64_t);

typedef struct {
    size_t index;
    uint32_t tag;
    bool used;
} VictimCache;

typedef struct cuckoo_table {
    size_t bits_per_item;   // bits per item
    uint32_t num_items;     // number of current item
    VictimCache victim;

    uint16_t dec_table[N_ENTS];
    uint16_t enc_table[1 << 16];

    size_t bits_per_tag;    // bits for tag
    size_t bits_per_bucket;     // bits for bucket
    size_t bytes_per_bucket;
    size_t bits_mask;

    size_t num_buckets;     // number of buckets
    // buckets len
    size_t len;
    char buckets[0];
} CuckooTable;

static inline uint64_t
upperpower2(uint64_t x) {
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    x |= x >> 32;
    x++;
    return x;
}

static inline float
max_load_factor(size_t tags_per_bucket) {
    switch(tags_per_bucket) {
    case 2:
        return 0.85;
    case 4:
        return 0.96;
    default:
        return 0.99;
    }
}

static inline void
unpack(uint16_t in, uint8_t *out) {
    out[0] = (uint8_t)(in & 0x000f);
    out[2] = (uint8_t)((in >> 4) & 0x000f);
    out[1] = (uint8_t)((in >> 8) & 0x000f);
    out[3] = (uint8_t)((in >> 12) & 0x000f);
}

/* pack four 4-bit numbers to one 2-byte number */
static inline uint16_t
pack(uint8_t *in) {
    uint16_t in1 = *((uint16_t *)(in)) & 0x0f0f;
    uint16_t in2 = *((uint16_t *)(in + 2)) << 4;
    return in1 | in2;
}

static inline void
gen_tables(CuckooTable *ct, int base, int k, uint8_t *dst, uint16_t *idx) {
    for (int i = base; i < 16; i++) {
        /* for fast comparison in binary_search in little-endian machine */
        dst[k] = i;
        if (k + 1 < 4) {
            gen_tables(ct, i, k + 1, dst, idx);
        } else {
            ct->dec_table[*idx] = pack(dst);
            ct->enc_table[pack(dst)] = *idx;
            (*idx)++;
        }
    }
}

static inline void
init_table(CuckooTable *ct) {
    uint8_t dst[4];
    uint16_t idx = 0;
    memset(ct->dec_table, 0, sizeof(ct->dec_table));
    memset(ct->enc_table, 0, sizeof(ct->enc_table));
    gen_tables(ct, 0, 0, dst, &idx);
}

static inline size_t
item_size(CuckooTable *ct) {
    size_t c = 0;
    if (ct->victim.used) {
        c = 1;
    }
    return ct->num_items + c;
}

static inline void
reset(CuckooTable *ct) {
    ct->num_items = 0;
    ct->victim.used = false;
    ct->victim.index = 0;
    ct->victim.tag = 0;
    memset(ct->buckets, 0, ct->len * sizeof(ct->buckets[0]));
}

/*
    tags_per_bucket: num of tags for each bucket, which is b in paper.
        tag is fingerprint, which is f in paper.
    bits_per_item: num of bits for each item, which is length of tag(fingerprint)
    total_size: num of keys that filter will store.
        this value should close to and lower than
        upperpower2(total_size/tags_per_bucket) * max_load_factor.
        cause num_buckets is always a power of two.
*/
static inline void
create_cuckoo_table(lua_State *L, size_t total_size, size_t tags_per_bucket,
        size_t bits_per_item) {
    size_t num_buckets = upperpower2((uint64_t)(total_size / tags_per_bucket));
    double frac = (double)total_size / (num_buckets * tags_per_bucket);
    if (frac > max_load_factor(tags_per_bucket)) {
        num_buckets <<= 1;
    }
    if (num_buckets == 0) {
        num_buckets = 1;
    }
    size_t bits_per_tag = bits_per_item - FPSIZE;
    size_t bits_per_bucket = (bits_per_item - 1) * TAGS_PER_TABLE;
    size_t bytes_per_bucket = (bits_per_bucket + 7) >> 3;
    // use 7 extra bytes to avoid overrun as we always read a uint64
    size_t len = ((bits_per_bucket * num_buckets + 7 ) >> 3) + 7;

    CuckooTable *ct = lua_newuserdatauv(L, sizeof(CuckooTable) +
            sizeof(ct->buckets[0]) * len, 0);
    ct->bits_per_item = bits_per_item;
    init_table(ct);
    ct->bits_per_tag = bits_per_tag;
    ct->bits_per_bucket = bits_per_bucket;
    ct->bytes_per_bucket = bytes_per_bucket;
    ct->bits_mask = ((1ULL << bits_per_tag) - 1) << FPSIZE;
    ct->num_buckets = num_buckets;
    ct->len = len;
    reset(ct);
}


static inline size_t
index_hash(uint32_t hv, size_t num_buckets) {
    // num_buckets is always a power of two, so modulo can be replaced with bitwise-and
    return hv & (num_buckets - 1);
}

static inline uint32_t
tag_hash(uint32_t hv, size_t bits_per_item) {
    uint32_t tag;
    tag = hv & ((1ULL << bits_per_item) - 1);
    tag += (tag == 0);
    return tag;
}

static inline void
generate_index_tag_hash(const char *str, size_t len, size_t *index, uint32_t *tag,
        size_t num_buckets, size_t bits_per_item) {
    uint64_t hash = komihash(str, len, 0);   // use default 0 seed
    // num_buckets is always a power of two, so modulo can be replaced with bitwise-and
    *index = index_hash((uint32_t)(hash >> 32), num_buckets);
    *tag = tag_hash((uint32_t)hash, bits_per_item);
}

static inline size_t
alt_index(size_t index, uint32_t tag, size_t num_buckets) {
    // doing a quick-n-dirty way:
    // 0x5bd1e995 is the hash constant from MurmurHash2
    return index_hash((uint32_t)index ^ (tag * 0x5bd1e995), num_buckets);
}

/* read and decode the bucket i, pass the 4 decoded tags to the 2nd arg
* bucket bits = 12 codeword bits + dir bits of tag1 + dir bits of tag2 ...
*/
static inline void
read_bucket(CuckooTable *ct, size_t i, uint32_t *tags) {
    const char *p;  // =  buckets_ + ((kBitsPerBucket * i) >> 3);
    uint16_t codeword;
    size_t bits_per_item = ct->bits_per_item;
    char *buckets = ct->buckets;
    size_t bits_mask = ct->bits_mask;
    if (bits_per_item == 5) {
        // 1 dirbits per tag, 16 bits per bucket
        p = buckets + (i * 2);
        uint16_t bucketbits = *((uint16_t *)p);
        codeword = bucketbits & 0x0fff;
        tags[0] = (uint32_t)(bucketbits >> 8) & bits_mask;
        tags[1] = (uint32_t)(bucketbits >> 9) & bits_mask;
        tags[2] = (uint32_t)(bucketbits >> 10) & bits_mask;
        tags[3] = (uint32_t)(bucketbits >> 11) & bits_mask;
    } else if (bits_per_item == 6) {
        // 2 dirbits per tag, 20 bits per bucket
        p = buckets + ((20 * i) >> 3);
        uint32_t bucketbits = *((uint32_t *)p);
        codeword = (uint16_t)bucketbits >> ((i & 1) << 2) & 0x0fff;
        tags[0] = (bucketbits >> (8 + ((i & 1) << 2))) & bits_mask;
        tags[1] = (bucketbits >> (10 + ((i & 1) << 2))) & bits_mask;
        tags[2] = (bucketbits >> (12 + ((i & 1) << 2))) & bits_mask;
        tags[3] = (bucketbits >> (14 + ((i & 1) << 2))) & bits_mask;
    } else if (bits_per_item == 7) {
        // 3 dirbits per tag, 24 bits per bucket
        p = buckets + (i << 1) + i;
        uint32_t bucketbits = *((uint32_t *)p);
        codeword = (uint16_t)bucketbits & 0x0fff;
        tags[0] = (bucketbits >> 8) & bits_mask;
        tags[1] = (bucketbits >> 11) & bits_mask;
        tags[2] = (bucketbits >> 14) & bits_mask;
        tags[3] = (bucketbits >> 17) & bits_mask;
    } else if (bits_per_item == 8) {
        // 4 dirbits per tag, 28 bits per bucket
        p = buckets + ((28 * i) >> 3);
        uint32_t bucketbits = *((uint32_t *)p);
        codeword = (uint16_t)bucketbits >> ((i & 1) << 2) & 0x0fff;
        tags[0] = (bucketbits >> (8 + ((i & 1) << 2))) & bits_mask;
        tags[1] = (bucketbits >> (12 + ((i & 1) << 2))) & bits_mask;
        tags[2] = (bucketbits >> (16 + ((i & 1) << 2))) & bits_mask;
        tags[3] = (bucketbits >> (20 + ((i & 1) << 2))) & bits_mask;
    } else if (bits_per_item == 9) {
        // 5 dirbits per tag, 32 bits per bucket
        p = buckets + (i * 4);
        uint32_t bucketbits = *((uint32_t *)p);
        codeword = (uint16_t)bucketbits & 0x0fff;
        tags[0] = (bucketbits >> 8) & bits_mask;
        tags[1] = (bucketbits >> 13) & bits_mask;
        tags[2] = (bucketbits >> 18) & bits_mask;
        tags[3] = (bucketbits >> 23) & bits_mask;
    } else if (bits_per_item == 13) {
        // 9 dirbits per tag,  48 bits per bucket
        p = buckets + (i * 6);
        uint64_t bucketbits = *((uint64_t *)p);
        codeword = (uint16_t)bucketbits & 0x0fff;
        tags[0] = (uint32_t)(bucketbits >> 8) & bits_mask;
        tags[1] = (uint32_t)(bucketbits >> 17) & bits_mask;
        tags[2] = (uint32_t)(bucketbits >> 26) & bits_mask;
        tags[3] = (uint32_t)(bucketbits >> 35) & bits_mask;
    } else if (bits_per_item == 17) {
        // 13 dirbits per tag, 64 bits per bucket
        p = buckets + (i << 3);
        uint64_t bucketbits = *((uint64_t *)p);
        codeword = (uint16_t)bucketbits & 0x0fff;
        tags[0] = (uint32_t)(bucketbits >> 8) & bits_mask;
        tags[1] = (uint32_t)(bucketbits >> 21) & bits_mask;
        tags[2] = (uint32_t)(bucketbits >> 34) & bits_mask;
        tags[3] = (uint32_t)(bucketbits >> 47) & bits_mask;
    } else {
        size_t bits_per_bucket = ct->bits_per_bucket;
        uint64_t u1 = 0, u2 = 0;
        size_t rshift = (bits_per_bucket * i) & (BITS_PER_BYTE - 1);
        // tag is max 32bit, store 31bit per tag, so max occupies 16 bytes
        size_t bytes = (rshift + bits_per_bucket + 7) / BITS_PER_BYTE;
        for (size_t k1 = 0; k1 < bytes; k1++) {
            p = buckets + ((bits_per_bucket * i) >> 3) + k1;
            if (k1 < BYTES_PER_UINT64) {
                u1 |= (uint64_t)(*((uint8_t *)p)) << (k1 * BITS_PER_BYTE);
            } else {
                u2 |= (uint64_t)(*((uint8_t *)p)) << ((k1 - BYTES_PER_UINT64) * BITS_PER_BYTE);
            }
        }
        codeword = (uint16_t)(u1 >> rshift) & 0x0fff;

        size_t bits_per_tag = ct->bits_per_tag;
        int shift;
        for (size_t k2 = 0; k2 < TAGS_PER_TABLE; k2++) {
            tags[k2] = (uint32_t)(u1 >> rshift >> (CODE_SIZE - FPSIZE + k2 * (int)bits_per_tag));
            shift = CODE_SIZE - FPSIZE + k2 * (int)bits_per_tag - 64 + (int)rshift;
            if (shift < 0) {
                tags[k2] |= (uint32_t)(u2 << -shift);
            } else {
                tags[k2] |= (uint32_t)(u2 >> shift);
            }
            tags[k2] &= bits_mask;
        }
    }

    uint8_t lowbits[TAGS_PER_TABLE];
    /* codeword is the lowest 12 bits in the bucket */
    unpack(ct->dec_table[codeword], lowbits);

    tags[0] |= lowbits[0];
    tags[1] |= lowbits[1];
    tags[2] |= lowbits[2];
    tags[3] |= lowbits[3];
}

static inline void
sort_pair(uint32_t *a, uint32_t *b) {
    if ((*a & 0x0f) > (*b & 0x0f)) {
        *a ^= *b;
        *b ^= *a;
        *a ^= *b;
    }
}

/* Tag = 4 low bits + x high bits
* L L L L H H H H ...
*/
static inline void
write_bucket(CuckooTable *ct, size_t i, uint32_t *tags) {
    /* first sort the tags in increasing order */
    sort_pair(&tags[0], &tags[2]);
    sort_pair(&tags[1], &tags[3]);
    sort_pair(&tags[0], &tags[1]);
    sort_pair(&tags[2], &tags[3]);
    sort_pair(&tags[1], &tags[2]);

    /* put in direct bits for each tag*/
    uint8_t lowbits[TAGS_PER_TABLE];
    uint32_t highbits[TAGS_PER_TABLE];

    lowbits[0] = (uint8_t)(tags[0] & 0x0f);
    lowbits[1] = (uint8_t)(tags[1] & 0x0f);
    lowbits[2] = (uint8_t)(tags[2] & 0x0f);
    lowbits[3] = (uint8_t)(tags[3] & 0x0f);

    highbits[0] = tags[0] & 0xfffffff0;
    highbits[1] = tags[1] & 0xfffffff0;
    highbits[2] = tags[2] & 0xfffffff0;
    highbits[3] = tags[3] & 0xfffffff0;

    // note that :  tags[j] = lowbits[j] | highbits[j]
    uint16_t codeword = ct->enc_table[pack(lowbits)];

    /* write out the bucketbits to its place*/
    size_t bits_per_bucket = ct->bits_per_bucket;
    char *p = ct->buckets + ((bits_per_bucket * i) >> 3);
    if (bits_per_bucket == 16) {
        // 1 dirbits per tag
        *((uint16_t *)p) = codeword | (highbits[0] << 8) | (highbits[1] << 9) |
                         (highbits[2] << 10) | (highbits[3] << 11);
    } else if (bits_per_bucket == 20) {
        // 2 dirbits per tag
        if ((i & 0x0001) == 0) {
            *((uint32_t *)p) &= 0xfff00000;
            *((uint32_t *)p) |= codeword | (highbits[0] << 8) |
                    (highbits[1] << 10) | (highbits[2] << 12) |
                    (highbits[3] << 14);
        } else {
            *((uint32_t *)p) &= 0xff00000f;
            *((uint32_t *)p) |= (codeword << 4) | (highbits[0] << 12) |
                    (highbits[1] << 14) | (highbits[2] << 16) |
                    (highbits[3] << 18);
        }
    } else if (bits_per_bucket == 24) {
        // 3 dirbits per tag
        *((uint32_t *)p) &= 0xff000000;
        *((uint32_t *)p) |= codeword | (highbits[0] << 8) | (highbits[1] << 11) |
                (highbits[2] << 14) | (highbits[3] << 17);
    } else if (bits_per_bucket == 28) {
        // 4 dirbits per tag
        if ((i & 0x0001) == 0) {
            *((uint32_t *)p) &= 0xf0000000;
            *((uint32_t *)p) |= codeword | (highbits[0] << 8) |
                    (highbits[1] << 12) | (highbits[2] << 16) |
                    (highbits[3] << 20);
        } else {
            *((uint32_t *)p) &= 0x0000000f;
            *((uint32_t *)p) |= (codeword << 4) | (highbits[0] << 12) |
                    (highbits[1] << 16) | (highbits[2] << 20) |
                    (highbits[3] << 24);
        }
    } else if (bits_per_bucket == 32) {
        // 5 dirbits per tag
        *((uint32_t *)p) = codeword | (highbits[0] << 8) | (highbits[1] << 13) |
                (highbits[2] << 18) | (highbits[3] << 23);
    } else if (bits_per_bucket == 48) {
        // 9 dirbits per tag
        *((uint64_t *)p) &= 0xffff000000000000ULL;
        *((uint64_t *)p) |= codeword | ((uint64_t)highbits[0] << 8) |
                ((uint64_t)highbits[1] << 17) | ((uint64_t)highbits[2] << 26) |
                ((uint64_t)highbits[3] << 35);
    } else if (bits_per_bucket == 64) {
        // 13 dirbits per tag
        *((uint64_t *)p) = codeword | ((uint64_t)highbits[0] << 8) |
                ((uint64_t)highbits[1] << 21) | ((uint64_t)highbits[2] << 34) |
                ((uint64_t)highbits[3] << 47);
    } else {
        size_t rshift = (bits_per_bucket * i) & (BITS_PER_BYTE - 1);
        size_t lshift = (rshift + bits_per_bucket) & (BITS_PER_BYTE - 1);
        // tag is max 32bit, store 31bit per tag, so max occupies 16 bytes
        size_t bytes = (rshift + bits_per_bucket + 7) / BITS_PER_BYTE;

        uint8_t rmask = (uint8_t)0xff >> (BITS_PER_BYTE - rshift);
        uint8_t lmask = (uint8_t)0xff << lshift;
        if (lshift == 0) {
            lmask = (uint8_t)0;
        }
        uint64_t u1 = 0, u2 = 0;
        u1 |= (uint64_t)(*((uint8_t *)p) & rmask);
        size_t end = bytes - 1;
        if (bytes > BYTES_PER_UINT64) {
            u2 |= (uint64_t)(*((uint8_t *)(p + end)) & lmask) << ((end - BYTES_PER_UINT64) * BITS_PER_BYTE);
        } else {
            u1 |= (uint64_t)(*((uint8_t *)(p + end)) & lmask) << (end * BITS_PER_BYTE);
        }

        size_t bits_per_tag = ct->bits_per_tag;
        int shift;
        u1 |= (uint64_t)codeword << rshift;
        for (size_t k1 = 0; k1 < TAGS_PER_TABLE; k1++) {
            u1 |= (uint64_t)highbits[k1] << (CODE_SIZE - FPSIZE + k1 * (int)bits_per_tag) << rshift;
            shift = CODE_SIZE - FPSIZE + k1 * (int)bits_per_tag - 64 + (int)rshift;
            if (shift < 0) {
                u2 |= (uint64_t)highbits[k1] >> -shift;
            } else {
                u2 |= (uint64_t)highbits[k1] << shift;
            }
        }
        for (size_t k2 = 0; k2 < bytes; k2++) {
            if (k2 < BYTES_PER_UINT64) {
                *((uint8_t *)(p + k2)) = (uint8_t)(u1 >> (k2 * BITS_PER_BYTE));
            } else {
                *((uint8_t *)(p + k2)) = (uint8_t)(u2 >> ((k2 - BYTES_PER_UINT64) * BITS_PER_BYTE));
            }
        }
    }
}

static inline bool
insert_tag_to_bucket(CuckooTable *ct, size_t i, uint32_t tag, bool kickout,
        uint32_t *oldtag) {
    uint32_t tags[TAGS_PER_TABLE];
    read_bucket(ct, i, tags);
    for (size_t j = 0; j < TAGS_PER_TABLE; j++) {
        if (tags[j] == 0) {
            tags[j] = tag;
            write_bucket(ct, i, tags);
            return true;
        }
    }
    if (kickout) {
        size_t r = rand() & (TAGS_PER_TABLE - 1);
        *oldtag = tags[r];
        tags[r] = tag;
        write_bucket(ct, i, tags);
    }
    return false;
}

static inline void
add_impl(CuckooTable *ct, size_t i, uint32_t tag) {
    size_t curindex = i;
    uint32_t curtag = tag;
    uint32_t oldtag;
    for (uint32_t count = 0; count < MAX_CUCKOO_COUNT; count++) {
        bool kickout = count > 0;
        oldtag = 0;
        if (insert_tag_to_bucket(ct, curindex, curtag, kickout, &oldtag)) {
            ct->num_items++;
            return;
        }
        if (kickout) {
            curtag = oldtag;
        }
        curindex = alt_index(curindex, curtag, ct->num_buckets);
    }
    ct->victim.index = curindex;
    ct->victim.tag = curtag;
    ct->victim.used = true;
    return;
}

// add an item into filter, return false when filter is full
static int
ladd(lua_State *L) {
    CuckooTable *ct = luaL_checkudata(L, 1, MT_NAME);
    // check for space
    if (ct->victim.used) {
        lua_pushboolean(L, 0);  // first result (false)
        lua_pushliteral(L, "not enough space");
        return 2;
    }
    size_t len;
    const char *str = luaL_checklstring(L, 2, &len);
    size_t i;
    uint32_t tag;
    generate_index_tag_hash(str, len, &i, &tag, ct->num_buckets, ct->bits_per_item);
    add_impl(ct, i, tag);
    lua_pushboolean(L, 1);
    return 1;
}

static bool
find_tag_in_buckets(CuckooTable *ct, size_t i1, size_t i2, uint32_t tag) {
    uint32_t tags1[TAGS_PER_TABLE];
    uint32_t tags2[TAGS_PER_TABLE];

    read_bucket(ct, i1, tags1);
    read_bucket(ct, i2, tags2);

    return (tags1[0] == tag) || (tags1[1] == tag) || (tags1[2] == tag) ||
           (tags1[3] == tag) || (tags2[0] == tag) || (tags2[1] == tag) ||
           (tags2[2] == tag) || (tags2[3] == tag);
}

// return if filter contains an item
static int
lcontain(lua_State *L) {
    CuckooTable *ct = luaL_checkudata(L, 1, MT_NAME);
    size_t len;
    const char *str = luaL_checklstring(L, 2, &len);
    size_t i1;
    uint32_t tag;
    generate_index_tag_hash(str, len, &i1, &tag, ct->num_buckets, ct->bits_per_item);
    size_t i2 = alt_index(i1, tag, ct->num_buckets);
    assert(i1 == alt_index(i2, tag, ct->num_buckets));
    bool found = ct->victim.used && (tag == ct->victim.tag) &&
            (i1 == ct->victim.index || i2 == ct->victim.index);
    if (found || find_tag_in_buckets(ct, i1, i2, tag)) {
        lua_pushboolean(L, 1);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

static bool
delete_tag_from_bucket(CuckooTable *ct, size_t i, uint32_t tag) {
    uint32_t tags[TAGS_PER_TABLE];
    read_bucket(ct, i, tags);
    for (size_t j = 0; j < TAGS_PER_TABLE; j++) {
        if (tags[j] == tag) {
            tags[j] = 0;
            write_bucket(ct, i, tags);
            return true;
        }
    }
    return false;
}

// delete item from filter, return false when item not exist
static int
ldelete(lua_State *L) {
    CuckooTable *ct = luaL_checkudata(L, 1, MT_NAME);
    size_t len;
    const char *str = luaL_checklstring(L, 2, &len);
    size_t i1;
    uint32_t tag;
    generate_index_tag_hash(str, len, &i1, &tag, ct->num_buckets, ct->bits_per_item);
    size_t i2 = alt_index(i1, tag, ct->num_buckets);
    if (delete_tag_from_bucket(ct, i1, tag) || delete_tag_from_bucket(ct, i2, tag)) {
        ct->num_items--;
        goto TryEliminateVictim;
    } else if (ct->victim.used && (tag == ct->victim.tag) && (i1 == ct->victim.index || i2 == ct->victim.index)) {
        ct->victim.used = false;
        lua_pushboolean(L, 1);
        return 1;
    } else {
        lua_pushboolean(L, 0);
        lua_pushliteral(L, "not found");
        return 2;
    }
TryEliminateVictim:
    if (ct->victim.used) {
        ct->victim.used = false;
        i1 = ct->victim.index;
        tag = ct->victim.tag;
        add_impl(ct, i1, tag);
    }
    lua_pushboolean(L, 1);
    return 1;
}

// return num of items that filter store
static int
lsize(lua_State *L) {
    CuckooTable *ct = luaL_checkudata(L, 1, MT_NAME);
    lua_pushinteger(L, item_size(ct));
    return 1;
}

// reset the fillter
static int
lreset(lua_State *L) {
    CuckooTable *ct = luaL_checkudata(L, 1, MT_NAME);
    reset(ct);
    return 0;
}

static int
linfo(lua_State *L) {
    CuckooTable *ct = luaL_checkudata(L, 1, MT_NAME);
    lua_createtable(L, 0, 8);

    // return bytes occupancy of table
    lua_pushinteger(L, ct->len);
    lua_setfield(L, -2, "hashtable_size");

    // return fingerprint size
    lua_pushinteger(L, ct->bits_per_item);
    lua_setfield(L, -2, "bits_per_item");

    // return bits occupancy per item of table
    lua_pushinteger(L, ct->bits_per_tag);
    lua_setfield(L, -2, "bits_per_tag");

    // return num of table buckets
    lua_pushinteger(L, ct->num_buckets);
    lua_setfield(L, -2, "num_buckets");

    // return num of tags that table can store
    lua_pushinteger(L, ct->num_buckets * TAGS_PER_TABLE);
    lua_setfield(L, -2, "table_can_hold_tags_number");

    // return load factor
    lua_pushnumber(L, 1.0 * item_size(ct) / (TAGS_PER_TABLE * ct->num_buckets));
    lua_setfield(L, -2, "load_factor");

    // return bits per item avg
    lua_pushnumber(L, 8.0 * (double)ct->len / item_size(ct));
    lua_setfield(L, -2, "bit/key");

    // return add item number
    lua_pushinteger(L, item_size(ct));
    lua_setfield(L, -2, "size");
    return 1;
}

static int
gc(lua_State *L) {
    // CuckooTable *ct = luaL_checkudata(L, 1, MT_NAME);
    return 0;
}

static int
lmetatable(lua_State *L) {
    if (luaL_newmetatable(L, MT_NAME)) {
        luaL_Reg l[] = {
            {"add", ladd},
            {"contain", lcontain},
            {"delete", ldelete},
            {"size", lsize},
            {"info", linfo},
            {"reset", lreset},
            { NULL, NULL }
        };
        luaL_newlib(L, l);
        lua_setfield(L, -2, "__index");

        lua_pushcfunction(L, gc);
        lua_setfield(L, -2, "__gc");
    }
    return 1;
}

// use packed bucket in default
static int
lnew(lua_State *L) {
    uint32_t total_size = luaL_checkinteger(L, 1);    // total size
    create_cuckoo_table(L,
            total_size,
            // luaL_optinteger(L, 2, 4),       // bucket size
            4,
            luaL_optinteger(L, 2, 16));     // fingerprint size
    lmetatable(L);
    lua_setmetatable(L, -2);
    return 1;
}

LUAMOD_API int
luaopen_cuckoofilter(lua_State *L) {
    luaL_checkversion(L);
    luaL_Reg l[] = {
        { "new", lnew },
        { NULL, NULL },
    };
    luaL_newlib(L, l);
    return 1;
}

#endif
