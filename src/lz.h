#ifndef LZ_H
#define LZ_H

#include <stdint.h>
#include <stddef.h>

/* Large window LZ engine - LZMA-class */
#define LZ_WINDOW_BITS  22          /* 4MB window */
#define LZ_WINDOW_SIZE  (1 << LZ_WINDOW_BITS)
#define LZ_WINDOW_MASK  (LZ_WINDOW_SIZE - 1)
#define LZ_HASH_BITS    20
#define LZ_HASH_SIZE    (1 << LZ_HASH_BITS)
#define LZ_HASH_MASK    (LZ_HASH_SIZE - 1)
#define LZ_MIN_MATCH    2
#define LZ_MAX_MATCH    273
#define LZ_BT_DEPTH     48          /* binary tree search depth */
#define LZ_NUM_REPS     4           /* recent distance slots */

/* Distance slots (like LZMA): distance -> slot + extra bits */
#define LZ_NUM_DIST_SLOTS 64

/* Optimal parsing */
#define LZ_OPT_LEN      4096        /* look-ahead for optimal parsing */

typedef struct {
    int      is_match;
    int      is_rep;        /* is this a rep-match? */
    int      rep_idx;       /* which rep slot (0-3) */
    uint8_t  literal;
    uint16_t length;
    uint32_t distance;      /* actual distance (1-based) */
} lz_token_t;

/* Binary tree match finder */
typedef struct {
    const uint8_t *data;
    size_t   size;
    size_t   pos;
    uint32_t *hash;         /* hash -> tree node */
    uint32_t *tree;         /* binary tree: [node*2]=left, [node*2+1]=right */
    uint32_t reps[LZ_NUM_REPS];  /* recent distances */
} lz_state_t;

/* Optimal parsing node */
typedef struct {
    uint32_t price;         /* cumulative bit cost to reach here */
    uint16_t len;           /* how we got here: match length (0=literal) */
    uint32_t dist;          /* distance (0=literal, 1+=match distance) */
    int      is_rep;
    int      rep_idx;
    uint32_t reps[LZ_NUM_REPS]; /* rep state at this position */
} lz_opt_node_t;

void lz_init(lz_state_t *lz, const uint8_t *data, size_t size);
void lz_free(lz_state_t *lz);

/* Find all matches at current position (returns count, fills matches[]) */
int lz_find_matches(lz_state_t *lz, lz_token_t *matches, int max_matches);

/* Find rep-matches at current position */
int lz_find_rep_matches(lz_state_t *lz, lz_token_t *reps);

/* Advance position (inserts bytes into hash/tree) */
void lz_advance(lz_state_t *lz, size_t count);

/* Advance position without tree inserts (use when tree already built) */
void lz_advance_skip(lz_state_t *lz, size_t count);

/* Update rep distances after encoding a match */
void lz_update_reps(lz_state_t *lz, uint32_t dist, int is_rep, int rep_idx);

/* Distance slot encoding */
uint32_t dist_to_slot(uint32_t dist);
uint32_t slot_to_dist_base(uint32_t slot);
uint32_t slot_extra_bits(uint32_t slot);

#endif /* LZ_H */
