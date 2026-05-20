#include "lz.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/*
 * Binary-tree match finder with 4MB window.
 * Uses 4-byte hashing into a binary tree for fast longest-match finding.
 * Supports rep-matches (4 recent distances reused cheaply).
 */

static inline uint32_t hash4(const uint8_t *p) {
    uint32_t v = *(const uint32_t *)p;  /* unaligned ok on x86 */
    return (v * 0x9E3779B1u) >> (32 - LZ_HASH_BITS);
}

void lz_init(lz_state_t *lz, const uint8_t *data, size_t size) {
    lz->data = data;
    lz->size = size;
    lz->pos = 0;
    lz->hash = malloc(LZ_HASH_SIZE * sizeof(uint32_t));
    lz->tree = calloc(LZ_WINDOW_SIZE * 2, sizeof(uint32_t));
    memset(lz->hash, 0xFF, LZ_HASH_SIZE * sizeof(uint32_t));  /* UINT32_MAX = empty */
    for (int i = 0; i < LZ_NUM_REPS; i++) lz->reps[i] = 1;
}

void lz_free(lz_state_t *lz) {
    free(lz->hash);
    free(lz->tree);
}

/* Insert current position into binary tree and find matches */
static int bt_insert_and_find(lz_state_t *lz, lz_token_t *matches, int max_matches) {
    int count = 0;
    if (lz->pos + 3 >= lz->size) return 0;

    uint32_t h = hash4(lz->data + lz->pos);
    uint32_t cur = (uint32_t)(lz->pos & LZ_WINDOW_MASK);
    uint32_t *left = &lz->tree[cur * 2];
    uint32_t *right = &lz->tree[cur * 2 + 1];

    uint32_t node = lz->hash[h];
    lz->hash[h] = (uint32_t)lz->pos;

    size_t max_len_left = 0, max_len_right = 0;
    int depth = LZ_BT_DEPTH;

    *left = UINT32_MAX;
    *right = UINT32_MAX;

    while (node != UINT32_MAX && depth-- > 0) {
        size_t node_pos = node;
        /* Check if node is within window */
        if (lz->pos - node_pos > LZ_WINDOW_SIZE - 1) break;
        if (node_pos >= lz->pos) break;  /* safety: can't reference future */

        uint32_t node_idx = node_pos & LZ_WINDOW_MASK;
        const uint8_t *a = lz->data + lz->pos;
        const uint8_t *b = lz->data + node_pos;

        size_t max_len = lz->size - lz->pos;
        if (max_len > LZ_MAX_MATCH) max_len = LZ_MAX_MATCH;

        /* Start comparing from the minimum of what we've already matched on each side */
        size_t start = (max_len_left < max_len_right) ? max_len_left : max_len_right;
        size_t len = start;
        /* Compare 8 bytes at a time */
        while (len + 8 <= max_len) {
            uint64_t va, vb;
            __builtin_memcpy(&va, a + len, 8);
            __builtin_memcpy(&vb, b + len, 8);
            if (va != vb) {
                len += __builtin_ctzll(va ^ vb) >> 3;
                goto bt_cmp_done;
            }
            len += 8;
        }
        while (len < max_len && a[len] == b[len]) len++;
        bt_cmp_done:

        if (len >= (size_t)LZ_MIN_MATCH && count < max_matches) {
            /* Record this match if it's longer than previous */
            if (count == 0 || len > matches[count-1].length) {
                matches[count].is_match = 1;
                matches[count].is_rep = 0;
                matches[count].rep_idx = 0;
                matches[count].length = (uint16_t)len;
                matches[count].distance = (uint32_t)(lz->pos - node_pos);
                count++;
                if (len == max_len) {
                    /* Can't do better, link and stop */
                    *left = lz->tree[node_idx * 2];
                    *right = lz->tree[node_idx * 2 + 1];
                    return count;
                }
            }
        }

        /* Binary tree insertion: compare and go left or right */
        if (len < max_len && a[len] < b[len]) {
            *right = (uint32_t)node_pos;
            right = &lz->tree[node_idx * 2];
            node = *right;
            max_len_right = len;
        } else {
            *left = (uint32_t)node_pos;
            left = &lz->tree[node_idx * 2 + 1];
            node = *left;
            max_len_left = len;
        }
    }

    *left = UINT32_MAX;
    *right = UINT32_MAX;
    return count;
}

int lz_find_matches(lz_state_t *lz, lz_token_t *matches, int max_matches) {
    return bt_insert_and_find(lz, matches, max_matches);
}

int lz_find_rep_matches(lz_state_t *lz, lz_token_t *reps) {
    int count = 0;
    if (lz->pos >= lz->size) return 0;

    for (int i = 0; i < LZ_NUM_REPS; i++) {
        uint32_t dist = lz->reps[i];
        if (dist == 0 || lz->pos < dist) continue;

        const uint8_t *a = lz->data + lz->pos;
        const uint8_t *b = lz->data + lz->pos - dist;
        size_t max_len = lz->size - lz->pos;
        if (max_len > LZ_MAX_MATCH) max_len = LZ_MAX_MATCH;

        size_t len = 0;
        while (len < max_len && a[len] == b[len]) len++;

        if (len >= (size_t)LZ_MIN_MATCH) {
            reps[count].is_match = 1;
            reps[count].is_rep = 1;
            reps[count].rep_idx = i;
            reps[count].length = (uint16_t)len;
            reps[count].distance = dist;
            count++;
        }
    }
    return count;
}

void lz_advance(lz_state_t *lz, size_t count) {
    /* Insert all skipped positions into the binary tree properly */
    for (size_t i = 1; i < count && lz->pos + i + 3 < lz->size; i++) {
        size_t p = lz->pos + i;
        uint32_t h = hash4(lz->data + p);
        uint32_t cur = (uint32_t)(p & LZ_WINDOW_MASK);
        uint32_t *left = &lz->tree[cur * 2];
        uint32_t *right = &lz->tree[cur * 2 + 1];
        uint32_t node = lz->hash[h];
        lz->hash[h] = (uint32_t)p;

        /* Abbreviated tree insert: limited depth for speed */
        int depth = 8;
        *left = UINT32_MAX;
        *right = UINT32_MAX;

        while (node != UINT32_MAX && depth-- > 0) {
            if (node >= p || p - node > LZ_WINDOW_SIZE - 1) break;
            uint32_t node_idx = node & LZ_WINDOW_MASK;
            const uint8_t *a = lz->data + p;
            const uint8_t *b = lz->data + node;
            size_t max_len = lz->size - p;
            if (max_len > 32) max_len = 32;  /* short comparison for speed */
            size_t len = 0;
            while (len < max_len && a[len] == b[len]) len++;

            if (len < max_len && a[len] < b[len]) {
                *right = (uint32_t)node;
                right = &lz->tree[node_idx * 2];
                node = *right;
            } else {
                *left = (uint32_t)node;
                left = &lz->tree[node_idx * 2 + 1];
                node = *left;
            }
        }
        *left = UINT32_MAX;
        *right = UINT32_MAX;
    }
    lz->pos += count;
}

void lz_advance_skip(lz_state_t *lz, size_t count) {
    lz->pos += count;
}

void lz_update_reps(lz_state_t *lz, uint32_t dist, int is_rep, int rep_idx) {
    if (is_rep) {
        /* Move rep_idx to front */
        uint32_t d = lz->reps[rep_idx];
        for (int i = rep_idx; i > 0; i--)
            lz->reps[i] = lz->reps[i-1];
        lz->reps[0] = d;
    } else {
        /* Push new distance to front */
        for (int i = LZ_NUM_REPS - 1; i > 0; i--)
            lz->reps[i] = lz->reps[i-1];
        lz->reps[0] = dist;
    }
}

/* --- Distance Slot Encoding --- */
/* Maps distance to a slot (like LZMA):
 * dist 1     -> slot 0
 * dist 2     -> slot 1
 * dist 3     -> slot 2
 * dist 4     -> slot 3
 * dist 5-6   -> slot 4-5 (1 extra bit)
 * dist 7-8   -> slot 6-7 (1 extra bit)
 * dist 9-12  -> slot 8-11 (2 extra bits)
 * ...
 * General: slot = 2*floor(log2(dist)) + bit below MSB, extra = slot/2 - 1 bits
 */
uint32_t dist_to_slot(uint32_t dist) {
    if (dist < 4) return dist;
    uint32_t msb = 31 - (uint32_t)__builtin_clz(dist);
    uint32_t slot = msb * 2 + ((dist >> (msb - 1)) & 1);
    return slot;
}

uint32_t slot_to_dist_base(uint32_t slot) {
    if (slot < 4) return slot;
    uint32_t msb = slot / 2;
    return ((2 | (slot & 1)) << (msb - 1));
}

uint32_t slot_extra_bits(uint32_t slot) {
    if (slot < 4) return 0;
    return slot / 2 - 1;
}
