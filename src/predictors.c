#include "krv.h"
#include <stdlib.h>
#include <string.h>

/* --- Shared helpers --- */
static inline uint32_t pred_from_counts(uint16_t n0, uint16_t n1) {
    return (uint32_t)((uint32_t)n1 * PROB_ONE / ((uint32_t)n0 + n1));
}

static inline void update_counts(uint16_t *c, int bit) {
    c[bit]++;
    if (c[bit] > 1000) {
        c[0] = (c[0] + 1) >> 1;
        c[1] = (c[1] + 1) >> 1;
    }
}

/* --- Order-0 --- */
void pred_o0_init(predictor_o0_t *p) {
    for (int i = 0; i < 256; i++) { p->counts[i][0] = 1; p->counts[i][1] = 1; }
}

uint32_t pred_o0_predict(predictor_o0_t *p, uint8_t ctx) {
    return pred_from_counts(p->counts[ctx][0], p->counts[ctx][1]);
}

void pred_o0_update(predictor_o0_t *p, uint8_t ctx, int bit) {
    update_counts(p->counts[ctx], bit);
}

/* --- Order-1 --- */
void pred_o1_init(predictor_o1_t *p) {
    for (int i = 0; i < 256; i++)
        for (int j = 0; j < 256; j++) { p->counts[i][j][0] = 1; p->counts[i][j][1] = 1; }
}

uint32_t pred_o1_predict(predictor_o1_t *p, uint8_t prev, uint8_t ctx) {
    return pred_from_counts(p->counts[prev][ctx][0], p->counts[prev][ctx][1]);
}

void pred_o1_update(predictor_o1_t *p, uint8_t prev, uint8_t ctx, int bit) {
    update_counts(p->counts[prev][ctx], bit);
}

/* --- Order-2 (hashed) --- */
static inline uint16_t o2_hash(uint8_t prev2, uint8_t prev1, uint8_t ctx) {
    uint32_t h = ((uint32_t)prev2 * 257 + (uint32_t)prev1) * 31 ^ ctx;
    return (uint16_t)(h & (O2_HASH_SIZE - 1));
}

void pred_o2_init(predictor_o2_t *p) {
    for (int i = 0; i < O2_HASH_SIZE; i++) { p->counts[i][0] = 1; p->counts[i][1] = 1; }
    p->cached_idx = 0;
}

uint32_t pred_o2_predict(predictor_o2_t *p, uint8_t prev2, uint8_t prev1, uint8_t ctx) {
    uint16_t h = o2_hash(prev2, prev1, ctx);
    p->cached_idx = h;
    return pred_from_counts(p->counts[h][0], p->counts[h][1]);
}

void pred_o2_update(predictor_o2_t *p, uint8_t prev2, uint8_t prev1, uint8_t ctx, int bit) {
    (void)prev2; (void)prev1; (void)ctx;
    update_counts(p->counts[p->cached_idx], bit);
}

/* --- Order-4 (hashed) --- */
static inline uint32_t o4_hash(const uint8_t *prev4, uint8_t ctx) {
    uint32_t v;
    __builtin_memcpy(&v, prev4, 4);
    /* Mix with ctx using multiply-xor-shift */
    uint32_t h = v ^ (v >> 16);
    h = h * 2654435761u;
    h ^= (uint32_t)ctx * 0x9e3779b9u;
    h ^= h >> 13;
    return h & (O4_HASH_SIZE - 1);
}

void pred_o4_init(predictor_o4_t *p) {
    for (int i = 0; i < O4_HASH_SIZE; i++) { p->counts[i][0] = 1; p->counts[i][1] = 1; }
    p->cached_idx = 0;
}

uint32_t pred_o4_predict(predictor_o4_t *p, const uint8_t *prev4, uint8_t ctx) {
    uint32_t h = o4_hash(prev4, ctx);
    p->cached_idx = h;
    return pred_from_counts(p->counts[h][0], p->counts[h][1]);
}

void pred_o4_update(predictor_o4_t *p, const uint8_t *prev4, uint8_t ctx, int bit) {
    (void)prev4; (void)ctx;
    update_counts(p->counts[p->cached_idx], bit);
}

/* --- Order-8 (hashed) --- */
static inline uint32_t o8_hash(const uint8_t *prev8, uint8_t ctx) {
    /* Use 6 bytes for faster hash while maintaining good distribution */
    uint32_t lo;
    uint16_t hi;
    __builtin_memcpy(&lo, prev8 + 2, 4);
    __builtin_memcpy(&hi, prev8, 2);
    uint32_t h = lo * 2654435761u;
    h ^= (uint32_t)hi * 0x9e3779b9u;
    h ^= (uint32_t)ctx * 16777619u;
    h ^= h >> 13;
    return h & (O8_HASH_SIZE - 1);
}

void pred_o8_init(predictor_o8_t *p) {
    for (int i = 0; i < O8_HASH_SIZE; i++) { p->counts[i][0] = 1; p->counts[i][1] = 1; }
    p->cached_idx = 0;
}

uint32_t pred_o8_predict(predictor_o8_t *p, const uint8_t *prev8, uint8_t ctx) {
    uint32_t h = o8_hash(prev8, ctx);
    p->cached_idx = h;
    return pred_from_counts(p->counts[h][0], p->counts[h][1]);
}

void pred_o8_update(predictor_o8_t *p, const uint8_t *prev8, uint8_t ctx, int bit) {
    (void)prev8; (void)ctx;
    update_counts(p->counts[p->cached_idx], bit);
}

/* --- Match Finder --- */
void pred_match_init(predictor_match_t *p) {
    p->window = calloc(MATCH_WINDOW, 1);
    p->hash_table = calloc(MATCH_HASH_SIZE, sizeof(uint32_t));
    p->pos = 0;
    p->match_pos = 0;
    p->match_len = 0;
    p->match_byte = 0;
    p->match_active = 0;
}

void pred_match_free(predictor_match_t *p) {
    free(p->window);
    free(p->hash_table);
}

static inline uint32_t match_hash3(const uint8_t *p) {
    return ((uint32_t)p[0] * 257 * 257 + (uint32_t)p[1] * 257 + p[2]) & (MATCH_HASH_SIZE - 1);
}

void pred_match_update_byte(predictor_match_t *p, uint8_t byte) {
    size_t wpos = p->pos & (MATCH_WINDOW - 1);
    p->window[wpos] = byte;
    p->pos++;

    p->match_active = 0;
    if (p->pos < 3) return;

    /* The 3-byte sequence ending 2 positions ago (for hash table insert) */
    size_t ins_start = (p->pos - 3) & (MATCH_WINDOW - 1);
    uint8_t s0 = p->window[ins_start];
    uint8_t s1 = p->window[(ins_start + 1) & (MATCH_WINDOW - 1)];
    uint8_t s2 = p->window[(ins_start + 2) & (MATCH_WINDOW - 1)];
    uint32_t h = ((uint32_t)s0 * 257 * 257 + (uint32_t)s1 * 257 + s2) & (MATCH_HASH_SIZE - 1);
    
    /* Look up before inserting (same hash = current 3-byte context) */
    uint32_t mpos = p->hash_table[h];
    p->hash_table[h] = (uint32_t)(p->pos - 3);

    /* Check if match is valid */
    if (mpos < p->pos - 3 && p->pos - mpos <= MATCH_WINDOW) {
        size_t mp = mpos & (MATCH_WINDOW - 1);
        if (p->window[mp] == s0 &&
            p->window[(mp+1) & (MATCH_WINDOW - 1)] == s1 &&
            p->window[(mp+2) & (MATCH_WINDOW - 1)] == s2) {
            p->match_byte = p->window[(mpos + 3) & (MATCH_WINDOW - 1)];
            p->match_active = 1;
        }
    }
}

uint32_t pred_match_predict(predictor_match_t *p, uint8_t ctx, int bit_idx) {
    if (!p->match_active) return PROB_HALF;
    /* Predict based on the expected bit from match_byte */
    int expected_bit = (p->match_byte >> bit_idx) & 1;
    /* Verify partial match: bits already decoded should match match_byte */
    int nbits = 0;
    uint8_t tmp = ctx;
    while (tmp > 1) { tmp >>= 1; nbits++; }
    /* Check if high bits match */
    uint8_t mask = (uint8_t)((0xFF << (8 - nbits)) & 0xFF);
    uint8_t decoded_high = (uint8_t)((ctx << (8 - nbits)) & 0xFF);
    uint8_t match_high = p->match_byte & mask;
    if (decoded_high != match_high) return PROB_HALF; /* match broken */

    return expected_bit ? (PROB_ONE - 256) : 256;  /* ~94% confidence */
}

/* --- Run-Length Predictor --- */
void pred_rl_init(predictor_rl_t *p) {
    p->run_byte = 0;
    p->run_length = 0;
    p->in_run = 0;
}

void pred_rl_update_byte(predictor_rl_t *p, uint8_t byte) {
    if (p->run_length > 0 && byte == p->run_byte) {
        p->run_length++;
        p->in_run = (p->run_length >= 2);
    } else {
        p->run_byte = byte;
        p->run_length = 1;
        p->in_run = 0;
    }
}

uint32_t pred_rl_predict(predictor_rl_t *p, uint8_t ctx, int bit_idx) {
    if (!p->in_run) return PROB_HALF;
    /* Predict continuation of run */
    int expected_bit = (p->run_byte >> bit_idx) & 1;
    /* Verify partial byte matches so far */
    int nbits = 0;
    uint8_t tmp = ctx;
    while (tmp > 1) { tmp >>= 1; nbits++; }
    uint8_t mask = (uint8_t)((0xFF << (8 - nbits)) & 0xFF);
    uint8_t decoded_high = (uint8_t)((ctx << (8 - nbits)) & 0xFF);
    uint8_t run_high = p->run_byte & mask;
    if (decoded_high != run_high) return PROB_HALF;

    /* Confidence scales with run length */
    uint32_t conf = 256 + (p->run_length > 10 ? 200 : p->run_length * 20);
    return expected_bit ? (PROB_ONE - conf) : conf;
}

/* --- Stride Predictor (Structural Resonance Detection) --- */

void pred_stride_init(predictor_stride_t *p) {
    memset(p->buf, 0, STRIDE_BUF_SIZE);
    p->pos = 0;
    p->stride = 0;
    p->confidence = 0;
    p->next_detect = STRIDE_DETECT_INTERVAL;
}

/* Autocorrelation-based stride detection — optimized */
static void stride_detect(predictor_stride_t *p) {
    if (p->pos < STRIDE_DETECT_MIN) return;

    size_t len = p->pos < STRIDE_BUF_SIZE ? p->pos : STRIDE_BUF_SIZE;
    uint32_t best_score = 0;
    uint32_t best_stride = 0;

    uint32_t max_stride = (uint32_t)(len / 4);
    if (max_stride > STRIDE_MAX) max_stride = STRIDE_MAX;
    uint32_t samples = len < 256 ? (uint32_t)(len - 2) : 256;

    for (uint32_t s = 2; s <= max_stride; s++) {
        uint32_t matches = 0;
        for (uint32_t i = 0; i < samples; i++) {
            size_t idx = (p->pos - 1 - i) & (STRIDE_BUF_SIZE - 1);
            size_t idx_s = (p->pos - 1 - i - s) & (STRIDE_BUF_SIZE - 1);
            if (p->buf[idx] == p->buf[idx_s]) matches++;
        }
        uint32_t score = (matches * 256) / samples;
        if (score > best_score + 2) {
            best_score = score;
            best_stride = s;
        }
        if (best_score > 204) break; /* >80% match — good enough */
    }

    if (best_score > 102 && best_stride > 0) {
        p->stride = best_stride;
        p->confidence = best_score;
    } else {
        p->stride = 0;
        p->confidence = 0;
    }
}

void pred_stride_update_byte(predictor_stride_t *p, uint8_t byte) {
    p->buf[p->pos & (STRIDE_BUF_SIZE - 1)] = byte;
    p->pos++;

    /* Periodically re-detect stride */
    if (p->pos >= p->next_detect) {
        stride_detect(p);
        p->next_detect = p->pos + STRIDE_DETECT_INTERVAL;
    }
}

uint32_t pred_stride_predict(predictor_stride_t *p, uint8_t ctx, int bit_idx) {
    if (p->stride == 0 || p->pos < p->stride) return PROB_HALF;

    /* Get the byte from one stride ago */
    size_t ref_idx = (p->pos - p->stride) & (STRIDE_BUF_SIZE - 1);
    uint8_t ref_byte = p->buf[ref_idx];

    /* Verify partial byte matches so far */
    int nbits = 0;
    uint8_t tmp = ctx;
    while (tmp > 1) { tmp >>= 1; nbits++; }
    if (nbits > 0) {
        uint8_t mask = (uint8_t)((0xFF << (8 - nbits)) & 0xFF);
        uint8_t decoded_high = (uint8_t)((ctx << (8 - nbits)) & 0xFF);
        if ((ref_byte & mask) != decoded_high) return PROB_HALF;
    }

    int expected_bit = (ref_byte >> bit_idx) & 1;

    /* Confidence based on autocorrelation strength */
    /* confidence is 0-256; map to probability offset */
    uint32_t conf = 128 + (p->confidence * 3 / 4); /* range ~128-320 */
    return expected_bit ? (PROB_ONE - conf) : conf;
}
