#include "krv.h"
#include "lz.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/*
 * Phase 4 block compression:
 * - 4MB window binary tree match finder with rep-matches
 * - Adaptive bit-model encoding for LZ tokens
 * - Context mixer for literal bytes
 * - Hybrid: tries pure CM and LZ+CM, picks smaller
 *
 * This combines Phase 3's proven CM with Phase 4's superior LZ engine.
 */

/* --- Adaptive bit model (fast, for token encoding) --- */
#define AB_BITS  11
#define AB_ONE   (1 << AB_BITS)
#define AB_HALF  (AB_ONE / 2)
#define AB_RATE  4

typedef struct { uint16_t prob; } abit_t;

static inline void ab_init(abit_t *a) { a->prob = AB_HALF; }
static inline uint32_t ab_p12(abit_t *a) { return (uint32_t)a->prob * 2; }
static inline void ab_update(abit_t *a, int bit) {
    if (bit) a->prob += (AB_ONE - a->prob) >> AB_RATE;
    else a->prob -= a->prob >> AB_RATE;
}
static inline void ab_enc(abit_t *a, ac_encoder_t *e, int bit) {
    ac_enc_bit(e, bit, ab_p12(a)); ab_update(a, bit);
}
static inline int ab_dec(abit_t *a, ac_decoder_t *d) {
    int bit = ac_dec_bit(d, ab_p12(a)); ab_update(a, bit); return bit;
}

/* Bit-tree encode/decode */
static void bt_enc(abit_t *tree, int bits, ac_encoder_t *e, uint32_t val) {
    uint32_t ctx = 1;
    for (int i = bits-1; i >= 0; i--) {
        int bit = (val >> i) & 1;
        ab_enc(&tree[ctx], e, bit);
        ctx = (ctx << 1) | bit;
    }
}
static uint32_t bt_dec(abit_t *tree, int bits, ac_decoder_t *d) {
    uint32_t ctx = 1;
    for (int i = 0; i < bits; i++) {
        int bit = ab_dec(&tree[ctx], d);
        ctx = (ctx << 1) | bit;
    }
    return ctx - (1u << bits);
}

/* --- LZ Token Model --- */
typedef struct {
    abit_t is_match[4];       /* [pos & 3] */
    abit_t is_rep;
    abit_t rep_idx[4];        /* which rep */
    abit_t len_tree[512];     /* length bit-tree for new-dist (9 bits) */
    abit_t rep_len_tree[512]; /* length bit-tree for rep-matches (9 bits) */
    abit_t dist_slot[64];     /* distance slot (6 bits) */
    abit_t dist_low[256];     /* low distance bits */
    abit_t dist_align[16];   /* alignment bits (4-bit tree for low 4 bits) */
    abit_t dist_special[128]; /* for slots 4-13: full adaptive encoding (up to 4 extra bits) */
} lz_model_t;

static void lz_model_init(lz_model_t *m) {
    abit_t *p = (abit_t *)m;
    for (size_t i = 0; i < sizeof(lz_model_t)/sizeof(abit_t); i++) ab_init(&p[i]);
}

/* (XOR-tree matched-literal tried and removed — CM with 9th predictor is better) */

/* --- Secondary Symbol Estimation (SSE/APM) --- */
/* Corrects systematic biases in mixer output.
 * Maps (quantized_probability, context) -> corrected probability.
 * Context = bit position (0-7) gives 8 separate correction tables. */
#define SSE_BINS     32   /* probability quantization bins */
#define SSE_CONTEXTS 8    /* one per bit position */

typedef struct {
    uint16_t table[SSE_CONTEXTS][SSE_BINS][2]; /* [ctx][bin][0/1 counts] */
} sse_t;

static void sse_init(sse_t *s) {
    for (int c = 0; c < SSE_CONTEXTS; c++)
        for (int b = 0; b < SSE_BINS; b++) {
            /* Initialize with high counts reflecting the bin's probability */
            /* This makes SSE nearly transparent initially */
            uint32_t p = (b * PROB_ONE + PROB_ONE/2) / SSE_BINS;
            s->table[c][b][1] = (uint16_t)(p * 64 / PROB_ONE + 1);
            s->table[c][b][0] = (uint16_t)((PROB_ONE - p) * 64 / PROB_ONE + 1);
        }
}

static inline uint32_t sse_predict(sse_t *s, uint32_t prob, int ctx) {
    /* Quantize probability to bin with interpolation */
    uint32_t scaled = prob * (SSE_BINS - 1);
    uint32_t bin = scaled / PROB_ONE;
    if (bin >= SSE_BINS - 1) bin = SSE_BINS - 2;
    uint32_t frac = scaled - bin * PROB_ONE; /* fractional part */

    /* Get predictions from adjacent bins */
    uint16_t *e0 = s->table[ctx][bin];
    uint16_t *e1 = s->table[ctx][bin + 1];
    uint32_t p0 = (uint32_t)e0[1] * PROB_ONE / ((uint32_t)e0[0] + e0[1]);
    uint32_t p1 = (uint32_t)e1[1] * PROB_ONE / ((uint32_t)e1[0] + e1[1]);

    /* Interpolate */
    uint32_t result = p0 + ((p1 - p0) * frac / PROB_ONE);
    if (result < 1) result = 1;
    if (result > PROB_ONE - 1) result = PROB_ONE - 1;
    return result;
}

static inline void sse_update(sse_t *s, uint32_t prob, int ctx, int bit) {
    uint32_t bin = prob * (SSE_BINS - 1) / PROB_ONE;
    if (bin >= SSE_BINS) bin = SSE_BINS - 1;
    s->table[ctx][bin][bit]++;
    if (s->table[ctx][bin][bit] > 1000) {
        s->table[ctx][bin][0] = (s->table[ctx][bin][0] + 1) >> 1;
        s->table[ctx][bin][1] = (s->table[ctx][bin][1] + 1) >> 1;
    }
}

/* --- Context Mixer State (from Phase 3) --- */
#define NUM_PRED_CM 9

/* -log2(p) * 8 in fixed point (1/8 bit units), indexed by prob>>4 (256 entries) */
static const uint8_t log2_table[256] = {
    96,96,80,72,64,60,56,53,51,48,46,45,43,42,40,39,
    38,37,36,35,35,34,33,33,32,31,31,30,30,29,29,28,
    28,27,27,27,26,26,25,25,25,24,24,24,23,23,23,22,
    22,22,22,21,21,21,21,20,20,20,20,19,19,19,19,19,
    18,18,18,18,18,17,17,17,17,17,16,16,16,16,16,16,
    15,15,15,15,15,15,15,14,14,14,14,14,14,14,13,13,
    13,13,13,13,13,13,12,12,12,12,12,12,12,12,12,11,
    11,11,11,11,11,11,11,11,11,10,10,10,10,10,10,10,
    10,10,10,10,9,9,9,9,9,9,9,9,9,9,9,9,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
    6,6,6,6,5,5,5,5,5,5,5,5,5,5,5,5,
    5,5,5,5,5,5,5,5,5,4,4,4,4,4,4,4,
    4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
    4,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,
};

typedef struct {
    predictor_o0_t *o0;
    predictor_o1_t *o1;
    predictor_o2_t *o2;
    predictor_o4_t *o4;
    predictor_o8_t *o8;
    predictor_match_t match;
    predictor_rl_t rl;
    predictor_stride_t *stride;
    mixer_t mix;
    uint8_t history[8];
    size_t byte_pos;
    uint8_t lz_match_byte;
    int lz_match_active;
} cm_state_t;

static int cm_init(cm_state_t *s) {
    s->o0 = calloc(1, sizeof(predictor_o0_t));
    s->o1 = calloc(1, sizeof(predictor_o1_t));
    s->o2 = calloc(1, sizeof(predictor_o2_t));
    s->o4 = calloc(1, sizeof(predictor_o4_t));
    s->o8 = calloc(1, sizeof(predictor_o8_t));
    s->stride = calloc(1, sizeof(predictor_stride_t));
    if (!s->o0 || !s->o1 || !s->o2 || !s->o4 || !s->o8 || !s->stride) return -1;
    pred_o0_init(s->o0); pred_o1_init(s->o1); pred_o2_init(s->o2);
    pred_o4_init(s->o4); pred_o8_init(s->o8);
    pred_match_init(&s->match); pred_rl_init(&s->rl);
    pred_stride_init(s->stride);
    mixer_init(&s->mix);
    memset(s->history, 0, 8);
    s->byte_pos = 0;
    s->lz_match_byte = 0;
    s->lz_match_active = 0;
    return 0;
}

static void cm_free(cm_state_t *s) {
    free(s->o0); free(s->o1); free(s->o2); free(s->o4); free(s->o8);
    free(s->stride);
    pred_match_free(&s->match);
}
static void cm_encode_byte(cm_state_t *s, ac_encoder_t *enc, uint8_t byte) {
    uint8_t ctx = 1;
    for (int i = 7; i >= 0; i--) {
        int bit = (byte >> i) & 1;
        s->mix.ctx = (7 - i) >> 2;  /* 0 for bits 7-4, 1 for bits 3-0 */
        uint32_t preds[NUM_PRED_CM];
        preds[0] = pred_o0_predict(s->o0, ctx);
        preds[1] = pred_o1_predict(s->o1, s->history[7], ctx);
        preds[2] = pred_o2_predict(s->o2, s->history[6], s->history[7], ctx);
        preds[3] = (s->byte_pos >= 4) ? pred_o4_predict(s->o4, s->history+4, ctx) : PROB_HALF;
        preds[4] = (s->byte_pos >= 8) ? pred_o8_predict(s->o8, s->history, ctx) : PROB_HALF;
        preds[5] = s->match.match_active ? pred_match_predict(&s->match, ctx, i) : PROB_HALF;
        preds[6] = s->rl.in_run ? pred_rl_predict(&s->rl, ctx, i) : PROB_HALF;
        preds[7] = s->stride->stride ? pred_stride_predict(s->stride, ctx, i) : PROB_HALF;
        preds[8] = s->lz_match_active ? ((s->lz_match_byte >> i) & 1 ? PROB_ONE*3/4 : PROB_ONE/4) : PROB_HALF;
        uint32_t prob = mixer_predict(&s->mix, preds);
        if (prob < 1) prob = 1; if (prob > PROB_ONE-1) prob = PROB_ONE-1;
        ac_enc_bit(enc, bit, prob);
        pred_o0_update(s->o0, ctx, bit);
        pred_o1_update(s->o1, s->history[7], ctx, bit);
        pred_o2_update(s->o2, s->history[6], s->history[7], ctx, bit);
        if (s->byte_pos >= 4) pred_o4_update(s->o4, s->history+4, ctx, bit);
        if (s->byte_pos >= 8) pred_o8_update(s->o8, s->history, ctx, bit);
        mixer_update(&s->mix, preds, bit);
        ctx = (ctx << 1) | bit;
    }
    pred_match_update_byte(&s->match, byte);
    pred_rl_update_byte(&s->rl, byte);
    pred_stride_update_byte(s->stride, byte);
    s->history[0] = s->history[1]; s->history[1] = s->history[2];
    s->history[2] = s->history[3]; s->history[3] = s->history[4];
    s->history[4] = s->history[5]; s->history[5] = s->history[6];
    s->history[6] = s->history[7];
    s->history[7] = byte;
    s->byte_pos++;
}

static uint8_t cm_decode_byte(cm_state_t *s, ac_decoder_t *dec) {
    uint8_t byte = 0, ctx = 1;
    for (int i = 7; i >= 0; i--) {
        s->mix.ctx = (7 - i) >> 2;  /* 0 for bits 7-4, 1 for bits 3-0 */
        uint32_t preds[NUM_PRED_CM];
        preds[0] = pred_o0_predict(s->o0, ctx);
        preds[1] = pred_o1_predict(s->o1, s->history[7], ctx);
        preds[2] = pred_o2_predict(s->o2, s->history[6], s->history[7], ctx);
        preds[3] = (s->byte_pos >= 4) ? pred_o4_predict(s->o4, s->history+4, ctx) : PROB_HALF;
        preds[4] = (s->byte_pos >= 8) ? pred_o8_predict(s->o8, s->history, ctx) : PROB_HALF;
        preds[5] = s->match.match_active ? pred_match_predict(&s->match, ctx, i) : PROB_HALF;
        preds[6] = s->rl.in_run ? pred_rl_predict(&s->rl, ctx, i) : PROB_HALF;
        preds[7] = s->stride->stride ? pred_stride_predict(s->stride, ctx, i) : PROB_HALF;
        preds[8] = s->lz_match_active ? ((s->lz_match_byte >> i) & 1 ? PROB_ONE*3/4 : PROB_ONE/4) : PROB_HALF;
        uint32_t prob = mixer_predict(&s->mix, preds);
        if (prob < 1) prob = 1; if (prob > PROB_ONE-1) prob = PROB_ONE-1;
        int bit = ac_dec_bit(dec, prob);
        pred_o0_update(s->o0, ctx, bit);
        pred_o1_update(s->o1, s->history[7], ctx, bit);
        pred_o2_update(s->o2, s->history[6], s->history[7], ctx, bit);
        if (s->byte_pos >= 4) pred_o4_update(s->o4, s->history+4, ctx, bit);
        if (s->byte_pos >= 8) pred_o8_update(s->o8, s->history, ctx, bit);
        mixer_update(&s->mix, preds, bit);
        ctx = (ctx << 1) | bit;
        byte = (byte << 1) | bit;
    }
    pred_match_update_byte(&s->match, byte);
    pred_rl_update_byte(&s->rl, byte);
    pred_stride_update_byte(s->stride, byte);
    s->history[0] = s->history[1]; s->history[1] = s->history[2];
    s->history[2] = s->history[3]; s->history[3] = s->history[4];
    s->history[4] = s->history[5]; s->history[5] = s->history[6];
    s->history[6] = s->history[7];
    s->history[7] = byte;
    s->byte_pos++;
    return byte;
}

static inline void cm_sync_byte(cm_state_t *s, uint8_t byte) {
    pred_match_update_byte(&s->match, byte);
    pred_rl_update_byte(&s->rl, byte);
    pred_stride_update_byte(s->stride, byte);
    s->history[0] = s->history[1]; s->history[1] = s->history[2];
    s->history[2] = s->history[3]; s->history[3] = s->history[4];
    s->history[4] = s->history[5]; s->history[5] = s->history[6];
    s->history[6] = s->history[7]; s->history[7] = byte;
    s->byte_pos++;
}

/* Batch sync for matched bytes — skips stride detection during match */
static inline void cm_sync_bytes(cm_state_t *s, const uint8_t *bytes, size_t len) {
    predictor_stride_t *st = s->stride;
    size_t saved_next = st->next_detect;
    /* Defer stride detection until after the match */
    st->next_detect = st->pos + len + STRIDE_DETECT_INTERVAL;
    for (size_t j = 0; j < len; j++) {
        uint8_t byte = bytes[j];
        pred_match_update_byte(&s->match, byte);
        pred_rl_update_byte(&s->rl, byte);
        /* Inline stride buffer write without detection */
        st->buf[st->pos & (STRIDE_BUF_SIZE - 1)] = byte;
        st->pos++;
        s->history[0] = s->history[1]; s->history[1] = s->history[2];
        s->history[2] = s->history[3]; s->history[3] = s->history[4];
        s->history[4] = s->history[5]; s->history[5] = s->history[6];
        s->history[6] = s->history[7]; s->history[7] = byte;
        s->byte_pos++;
    }
    /* Restore detection schedule — detect on next literal if overdue */
    if (st->pos >= saved_next) {
        st->next_detect = st->pos;  /* will trigger on next update_byte call */
    } else {
        st->next_detect = saved_next;
    }
}

/* Measure CM entropy cost for a byte without encoding (returns cost in 1/8 bit units) */
static uint32_t cm_measure_byte(cm_state_t *s, uint8_t byte) {
    uint8_t ctx = 1;
    uint32_t cost = 0;
    for (int i = 7; i >= 0; i--) {
        int bit = (byte >> i) & 1;
        s->mix.ctx = (7 - i) >> 2;  /* 0 for bits 7-4, 1 for bits 3-0 */
        uint32_t preds[NUM_PRED_CM];
        preds[0] = pred_o0_predict(s->o0, ctx);
        preds[1] = pred_o1_predict(s->o1, s->history[7], ctx);
        preds[2] = pred_o2_predict(s->o2, s->history[6], s->history[7], ctx);
        preds[3] = (s->byte_pos >= 4) ? pred_o4_predict(s->o4, s->history+4, ctx) : PROB_HALF;
        preds[4] = (s->byte_pos >= 8) ? pred_o8_predict(s->o8, s->history, ctx) : PROB_HALF;
        preds[5] = s->match.match_active ? pred_match_predict(&s->match, ctx, i) : PROB_HALF;
        preds[6] = s->rl.in_run ? pred_rl_predict(&s->rl, ctx, i) : PROB_HALF;
        preds[7] = s->stride->stride ? pred_stride_predict(s->stride, ctx, i) : PROB_HALF;
        preds[8] = s->lz_match_active ? ((s->lz_match_byte >> i) & 1 ? PROB_ONE*3/4 : PROB_ONE/4) : PROB_HALF;
        uint32_t prob = mixer_predict(&s->mix, preds);
        if (prob < 1) prob = 1; if (prob > PROB_ONE-1) prob = PROB_ONE-1;
        /* Compute -log2(prob_of_actual_bit) */
        uint32_t p = bit ? prob : (PROB_ONE - prob);
        /* p is in 0..4096, convert to 1/8 bit cost via log2_table */
        if (p < 16) p = 16;
        cost += log2_table[p >> 4];
        /* Update predictors (same as encode) */
        pred_o0_update(s->o0, ctx, bit);
        pred_o1_update(s->o1, s->history[7], ctx, bit);
        pred_o2_update(s->o2, s->history[6], s->history[7], ctx, bit);
        if (s->byte_pos >= 4) pred_o4_update(s->o4, s->history+4, ctx, bit);
        if (s->byte_pos >= 8) pred_o8_update(s->o8, s->history, ctx, bit);
        mixer_update(&s->mix, preds, bit);
        ctx = (ctx << 1) | bit;
    }
    pred_match_update_byte(&s->match, byte);
    pred_rl_update_byte(&s->rl, byte);
    pred_stride_update_byte(s->stride, byte);
    s->history[0] = s->history[1]; s->history[1] = s->history[2];
    s->history[2] = s->history[3]; s->history[3] = s->history[4];
    s->history[4] = s->history[5]; s->history[5] = s->history[6];
    s->history[6] = s->history[7];
    s->history[7] = byte;
    s->byte_pos++;
    return cost;
}

/* --- Method 0: Pure Context Mixing --- */
static size_t compress_m0(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size) {
    cm_state_t s; if (cm_init(&s) != 0) return 0;
    ac_encoder_t enc; ac_enc_init(&enc, out, out_size);
    for (size_t i = 0; i < in_size; i++) cm_encode_byte(&s, &enc, in[i]);
    size_t r = ac_enc_flush(&enc); cm_free(&s); return r;
}

static size_t decompress_m0(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size) {
    cm_state_t s; if (cm_init(&s) != 0) return 0;
    ac_decoder_t dec; ac_dec_init(&dec, in, in_size);
    for (size_t i = 0; i < out_size; i++) out[i] = cm_decode_byte(&s, &dec);
    cm_free(&s); return out_size;
}

/* --- Method 1: LZ + CM (4MB window, BT4, optimal parsing with rep-state tracking) --- */

#define OPT_WINDOW 4194304
#define OPT_PRICE_INF 0x3FFFFFFF
#define OPT_MAX_MATCHES 8  /* max BT matches stored per position */

typedef struct {
    uint32_t price;
    uint16_t len;           /* 0 = literal */
    uint32_t dist;
    int8_t   rep_idx;       /* -1 = new dist, 0-3 = rep */
    uint32_t reps[LZ_NUM_REPS];
} opt_node_t;

typedef struct {
    uint16_t len;
    uint32_t dist;
} opt_match_entry_t;

/* Price estimation using adaptive bit model probabilities */

static inline uint32_t ab_price(abit_t *a, int bit) {
    uint32_t p = ab_p12(a); /* 0..4096 */
    if (!bit) p = 4096 - p;
    if (p < 16) p = 16;
    return log2_table[p >> 4];
}

/* Price of encoding a value through a bit-tree */
static inline uint32_t bt_price(abit_t *tree, int bits, uint32_t val) {
    uint32_t p = 0, ctx = 1;
    for (int i = bits-1; i >= 0; i--) {
        int bit = (val >> i) & 1;
        p += ab_price(&tree[ctx], bit);
        ctx = (ctx << 1) | bit;
    }
    return p;
}

/* Price of encoding a rep-match (without length — caller adds per-length cost) */
static inline uint32_t price_rep(lz_model_t *lm, uint32_t pos, int rep_idx, int len) {
    (void)len;
    uint32_t p = 0;
    p += ab_price(&lm->is_match[pos & 3], 1);
    p += ab_price(&lm->is_rep, 1);
    p += ab_price(&lm->rep_idx[0], rep_idx >= 1);
    if (rep_idx >= 1) p += ab_price(&lm->rep_idx[1], rep_idx >= 2);
    if (rep_idx >= 2) p += ab_price(&lm->rep_idx[2], rep_idx >= 3);
    return p;
}

/* Price of encoding a new-distance match (without length — caller adds per-length cost) */
static inline uint32_t price_newdist(lz_model_t *lm, uint32_t pos, uint32_t dist, int len) {
    (void)len;
    uint32_t p = 0;
    p += ab_price(&lm->is_match[pos & 3], 1);
    p += ab_price(&lm->is_rep, 0);
    uint32_t slot = dist_to_slot(dist);
    p += bt_price(lm->dist_slot, 6, slot);
    uint32_t extra = slot_extra_bits(slot);
    if (extra <= 4 && extra > 0) {
        uint32_t base = slot_to_dist_base(slot);
        uint32_t rem = dist - base;
        uint32_t cb = (slot - 4) * 16, tc = 1;
        for (uint32_t b = extra; b > 0; b--) {
            int bit = (rem >> (b-1)) & 1;
            p += ab_price(&lm->dist_special[cb + tc], bit);
            tc = (tc << 1) | bit;
        }
    } else if (extra > 4) {
        uint32_t base = slot_to_dist_base(slot);
        uint32_t rem = dist - base;
        p += (extra - 4) * 8;
        uint32_t l4 = rem & 0xF, tc = 1;
        for (int b = 3; b >= 0; b--) {
            int bit = (l4 >> b) & 1;
            p += ab_price(&lm->dist_align[tc], bit);
            tc = (tc << 1) | bit;
        }
    }
    return p;
}

/* Update reps array (non-destructive, returns new state) */
static inline void reps_after_match(const uint32_t *old_reps, uint32_t dist, int rep_idx, uint32_t *new_reps) {
    if (rep_idx >= 0) {
        /* Move rep_idx to front */
        new_reps[0] = old_reps[rep_idx];
        for (int i = 1; i <= rep_idx; i++) new_reps[i] = old_reps[i-1];
        for (int i = rep_idx + 1; i < LZ_NUM_REPS; i++) new_reps[i] = old_reps[i];
    } else {
        /* Push new distance to front */
        new_reps[0] = dist;
        for (int i = 1; i < LZ_NUM_REPS; i++) new_reps[i] = old_reps[i-1];
    }
}

static size_t compress_m1(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size) {
    cm_state_t cm; if (cm_init(&cm) != 0) return 0;
    /* Use full CM pricing for very small blocks (< 4KB) where accuracy matters most */
    int use_cm_pricing = (in_size <= 4096);
    cm_state_t price_cm;
    if (use_cm_pricing) { if (cm_init(&price_cm) != 0) { cm_free(&cm); return 0; } }
    lz_model_t *lm = calloc(1, sizeof(lz_model_t));
    if (!lm) { cm_free(&cm); if (use_cm_pricing) cm_free(&price_cm); return 0; }
    lz_model_init(lm);

    ac_encoder_t enc; ac_enc_init(&enc, out, out_size);
    lz_state_t lz; lz_init(&lz, in, in_size);

    /* Allocate DP structures — sized to actual data, not max window */
    size_t dp_window = in_size < OPT_WINDOW ? in_size : OPT_WINDOW;
    opt_node_t *opt = calloc(dp_window + LZ_MAX_MATCH + 1, sizeof(opt_node_t));
    /* Match cache: for each position in window, store up to OPT_MAX_MATCHES BT matches */
    opt_match_entry_t *mcache = malloc((size_t)dp_window * OPT_MAX_MATCHES * sizeof(opt_match_entry_t));
    uint8_t *mcache_count = calloc(dp_window, 1);
    /* Per-position literal cost from fast estimator or CM measurement */
    uint8_t *lit_costs = malloc(dp_window);
    if (!opt || !mcache || !mcache_count || !lit_costs) {
        free(opt); free(mcache); free(mcache_count); free(lit_costs);
        lz_free(&lz); cm_free(&cm); if (use_cm_pricing) cm_free(&price_cm); free(lm);
        return 0;
    }

    /* Fast order-2 frequency estimator (used when !use_cm_pricing) */
    uint16_t (*o1_freq)[256] = NULL;
    uint32_t o1_total[256];
    #define O2P_SIZE 65536
    typedef struct { uint16_t counts[256]; uint16_t total; } o2p_ctx_t;
    o2p_ctx_t *o2_freq = NULL;
    if (!use_cm_pricing) {
        o1_freq = calloc(256, 256 * sizeof(uint16_t));
        o2_freq = calloc(O2P_SIZE, sizeof(o2p_ctx_t));
        if (!o1_freq || !o2_freq) {
            free(opt); free(mcache); free(mcache_count); free(lit_costs);
            free(o1_freq); free(o2_freq);
            lz_free(&lz); cm_free(&cm); free(lm);
            return 0;
        }
        for (int c = 0; c < 256; c++) {
            for (int s = 0; s < 256; s++) o1_freq[c][s] = 1;
            o1_total[c] = 256;
        }
    }

    while (lz.pos < in_size) {
        size_t chunk_start = lz.pos;
        size_t chunk_end = chunk_start + dp_window;
        if (chunk_end > in_size) chunk_end = in_size;
        size_t chunk_len = chunk_end - chunk_start;

        /* === Pass 1: Collect matches by advancing BT linearly === */
        /* Also estimate per-position literal cost from local byte statistics */
        size_t saved_pos = lz.pos;
        uint32_t saved_reps[LZ_NUM_REPS];
        memcpy(saved_reps, lz.reps, sizeof(saved_reps));

        /* Estimate per-position literal cost */
        if (use_cm_pricing) {
            /* Full CM pricing for small blocks (accurate) */
            for (size_t i = 0; i < chunk_len; i++) {
                uint32_t cost = cm_measure_byte(&price_cm, in[chunk_start + i]);
                if (cost > 255) cost = 255;
                if (cost < 4) cost = 4;
                lit_costs[i] = (uint8_t)cost;
            }
        } else {
            /* Fast order-2/order-1 estimator for large blocks */
            uint8_t prev = (chunk_start > 0) ? in[chunk_start - 1] : 0;
            uint8_t prev2 = (chunk_start > 1) ? in[chunk_start - 2] : 0;
            for (size_t i = 0; i < chunk_len; i++) {
                uint8_t byte = in[chunk_start + i];
                uint32_t o2h = ((uint32_t)prev2 * 257 + prev) & (O2P_SIZE - 1);
                o2p_ctx_t *o2c = &o2_freq[o2h];
                uint32_t p;
                if (o2c->total >= 32) {
                    p = ((uint32_t)o2c->counts[byte] + 1) * 4096 / (o2c->total + 256);
                } else {
                    p = (uint32_t)o1_freq[prev][byte] * 4096 / o1_total[prev];
                }
                if (p < 16) p = 16;
                if (p > 4080) p = 4080;
                uint32_t cost = log2_table[p >> 4];
                if (cost < 4) cost = 4;
                if (cost > 255) cost = 255;
                lit_costs[i] = (uint8_t)cost;
                o1_freq[prev][byte] += 4;
                o1_total[prev] += 4;
                if (o1_total[prev] > 8192) {
                    o1_total[prev] = 0;
                    for (int s = 0; s < 256; s++) {
                        o1_freq[prev][s] = (o1_freq[prev][s] + 1) >> 1;
                        o1_total[prev] += o1_freq[prev][s];
                    }
                }
                o2c->counts[byte] += 2;
                o2c->total += 2;
                if (o2c->total > 4096) {
                    o2c->total = 0;
                    for (int s = 0; s < 256; s++) {
                        o2c->counts[s] = (o2c->counts[s] + 1) >> 1;
                        o2c->total += o2c->counts[s];
                    }
                }
                prev2 = prev;
                prev = byte;
            }
        }

        memset(mcache_count, 0, chunk_len);
        for (size_t i = 0; i < chunk_len; i++) {
            if (lz.pos + 3 < in_size) {
                lz_token_t matches[8];
                int nm = lz_find_matches(&lz, matches, 8);
                int stored = 0;
                for (int j = 0; j < nm && stored < OPT_MAX_MATCHES; j++) {
                    if (matches[j].length >= LZ_MIN_MATCH) {
                        mcache[i * OPT_MAX_MATCHES + stored].len = matches[j].length;
                        mcache[i * OPT_MAX_MATCHES + stored].dist = matches[j].distance;
                        stored++;
                    }
                }
                mcache_count[i] = stored;
            }
            lz.pos++;
        }

        /* Reset LZ position for emit pass (tree is populated, that's fine) */
        lz.pos = saved_pos;
        memcpy(lz.reps, saved_reps, sizeof(saved_reps));

        /* === Pass 2: Forward DP with full rep-state tracking === */
        opt[0].price = 0;
        opt[0].len = 0;
        memcpy(opt[0].reps, lz.reps, sizeof(lz.reps));
        for (size_t i = 1; i <= chunk_len; i++)
            opt[i].price = OPT_PRICE_INF;

        for (size_t i = 0; i < chunk_len; i++) {
            if (opt[i].price == OPT_PRICE_INF) continue;
            uint32_t cur_pos = chunk_start + i;

            /* Option 1: Literal — use CM-measured cost */
            uint32_t lcost = lit_costs[i];
            uint32_t lp = opt[i].price + ab_price(&lm->is_match[cur_pos & 3], 0) + lcost;
            if (lp < opt[i+1].price) {
                opt[i+1].price = lp;
                opt[i+1].len = 0;
                opt[i+1].dist = 0;
                opt[i+1].rep_idx = -1;
                memcpy(opt[i+1].reps, opt[i].reps, sizeof(opt[i].reps));
            }

            /* Option 2: Rep-matches (using tracked rep state) */
            for (int r = 0; r < LZ_NUM_REPS; r++) {
                uint32_t d = opt[i].reps[r];
                if (d == 0 || cur_pos < d) continue;
                const uint8_t *a = in + cur_pos;
                const uint8_t *b = in + cur_pos - d;
                size_t max_l = in_size - cur_pos;
                if (max_l > LZ_MAX_MATCH) max_l = LZ_MAX_MATCH;
                /* Quick check: if first 2 bytes don't match, skip */
                if (a[0] != b[0] || a[1] != b[1]) continue;
                size_t ml = 2;
                /* Compare 8 bytes at a time */
                while (ml + 8 <= max_l) {
                    uint64_t va, vb;
                    __builtin_memcpy(&va, a + ml, 8);
                    __builtin_memcpy(&vb, b + ml, 8);
                    if (va != vb) {
                        ml += __builtin_ctzll(va ^ vb) >> 3;
                        goto rep_done;
                    }
                    ml += 8;
                }
                while (ml < max_l && a[ml] == b[ml]) ml++;
                rep_done:
                if ((int)ml < LZ_MIN_MATCH) continue;

                /* Try strategic lengths: min, powers of 2, and max */
                uint32_t base_p = price_rep(lm, cur_pos, r, (int)ml);
                size_t try_lens[] = {LZ_MIN_MATCH, 3, 4, 5, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 273};
                for (int ti = 0; ti < 16; ti++) {
                    size_t l = try_lens[ti];
                    if (l < (size_t)LZ_MIN_MATCH) continue;
                    if (l > ml) l = ml;
                    size_t dest = i + l;
                    if (dest > chunk_len) break;
                    uint32_t tp = opt[i].price + base_p + bt_price(lm->rep_len_tree, 9, l - LZ_MIN_MATCH);
                    if (tp < opt[dest].price) {
                        opt[dest].price = tp;
                        opt[dest].len = l;
                        opt[dest].dist = d;
                        opt[dest].rep_idx = r;
                        reps_after_match(opt[i].reps, d, r, opt[dest].reps);
                    }
                    if (l == ml) break;
                }
            }

            /* Option 3: New-distance matches from cache */
            int nm = mcache_count[i];
            for (int j = 0; j < nm; j++) {
                uint32_t dist = mcache[i * OPT_MAX_MATCHES + j].dist;
                uint16_t mlen = mcache[i * OPT_MAX_MATCHES + j].len;
                if (mlen < LZ_MIN_MATCH) continue;

                /* Skip if this distance is already a rep (DP will find it cheaper as rep) */
                int is_rep = 0;
                for (int r = 0; r < LZ_NUM_REPS; r++)
                    if (opt[i].reps[r] == dist) { is_rep = 1; break; }
                if (is_rep) continue;

                uint32_t base_p = price_newdist(lm, cur_pos, dist, mlen);
                size_t try_lens[] = {LZ_MIN_MATCH, 3, 4, 5, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 273};
                for (int ti = 0; ti < 16; ti++) {
                    size_t l = try_lens[ti];
                    if (l < (size_t)LZ_MIN_MATCH) continue;
                    if (l > mlen) l = mlen;
                    size_t dest = i + l;
                    if (dest > chunk_len) break;
                    uint32_t tp = opt[i].price + base_p + bt_price(lm->len_tree, 9, l - LZ_MIN_MATCH);
                    if (tp < opt[dest].price) {
                        opt[dest].price = tp;
                        opt[dest].len = l;
                        opt[dest].dist = dist;
                        opt[dest].rep_idx = -1;
                        reps_after_match(opt[i].reps, dist, -1, opt[dest].reps);
                    }
                    if (l == (size_t)mlen) break;
                }
            }
        }

        /* === Pass 3: Backward trace, then forward emit === */
        /* Trace backward from chunk_len */
        size_t path_len = 0;
        /* Use a stack (path_buf stores indices from end to start) */
        uint32_t *path_pos = (uint32_t *)malloc(chunk_len * sizeof(uint32_t));
        {
            size_t p = chunk_len;
            while (p > 0) {
                path_pos[path_len++] = p;
                p -= (opt[p].len > 0) ? opt[p].len : 1;
            }
        }

        /* Emit forward (path is reversed, so iterate backward) */
        size_t emit_pos = chunk_start;
        uint32_t last_match_dist = 0;
        for (size_t pi = path_len; pi > 0; pi--) {
            opt_node_t *nd = &opt[path_pos[pi-1]];
            uint32_t ps = emit_pos & 3;

            if (nd->len == 0) {
                /* Literal */
                ab_enc(&lm->is_match[ps], &enc, 0);
                if (last_match_dist > 0 && emit_pos >= last_match_dist) {
                    cm.lz_match_byte = in[emit_pos - last_match_dist];
                    cm.lz_match_active = 1;
                } else {
                    cm.lz_match_active = 0;
                }
                cm_encode_byte(&cm, &enc, in[emit_pos]);
                lz_advance_skip(&lz, 1);
                emit_pos++;
            } else {
                /* Match — verify rep status against actual lz.reps */
                int actual_rep = -1;
                if (nd->rep_idx >= 0) {
                    /* Verify the rep is still valid */
                    if (lz.reps[nd->rep_idx] == nd->dist)
                        actual_rep = nd->rep_idx;
                    else {
                        for (int r = 0; r < LZ_NUM_REPS; r++)
                            if (lz.reps[r] == nd->dist) { actual_rep = r; break; }
                    }
                } else {
                    for (int r = 0; r < LZ_NUM_REPS; r++)
                        if (lz.reps[r] == nd->dist) { actual_rep = r; break; }
                }

                last_match_dist = nd->dist;
                ab_enc(&lm->is_match[ps], &enc, 1);
                if (actual_rep >= 0) {
                    ab_enc(&lm->is_rep, &enc, 1);
                    ab_enc(&lm->rep_idx[0], &enc, actual_rep >= 1);
                    if (actual_rep >= 1) ab_enc(&lm->rep_idx[1], &enc, actual_rep >= 2);
                    if (actual_rep >= 2) ab_enc(&lm->rep_idx[2], &enc, actual_rep >= 3);
                    uint32_t lv = nd->len - LZ_MIN_MATCH;
                    bt_enc(lm->rep_len_tree, 9, &enc, lv > 511 ? 511 : lv);
                } else {
                    ab_enc(&lm->is_rep, &enc, 0);
                    uint32_t lv = nd->len - LZ_MIN_MATCH;
                    bt_enc(lm->len_tree, 9, &enc, lv > 511 ? 511 : lv);
                    uint32_t slot = dist_to_slot(nd->dist);
                    bt_enc(lm->dist_slot, 6, &enc, slot);
                    uint32_t extra = slot_extra_bits(slot);
                    if (extra > 0) {
                        uint32_t base = slot_to_dist_base(slot);
                        uint32_t rem = nd->dist - base;
                        if (extra <= 4) {
                            uint32_t cb = (slot - 4) * 16, tc = 1;
                            for (uint32_t b = extra; b > 0; b--) {
                                int bit = (rem >> (b-1)) & 1;
                                ab_enc(&lm->dist_special[cb + tc], &enc, bit);
                                tc = (tc << 1) | bit;
                            }
                        } else {
                            uint32_t hb = extra - 4, hv = rem >> 4;
                            for (uint32_t b = hb; b > 0; b--)
                                ac_enc_bit(&enc, (hv >> (b-1)) & 1, PROB_HALF);
                            uint32_t l4 = rem & 0xF, tc = 1;
                            for (int b = 3; b >= 0; b--) {
                                int bit = (l4 >> b) & 1;
                                ab_enc(&lm->dist_align[tc], &enc, bit);
                                tc = (tc << 1) | bit;
                            }
                        }
                    }
                }
                cm_sync_bytes(&cm, in + emit_pos, nd->len);
                lz_update_reps(&lz, nd->dist, actual_rep >= 0, actual_rep >= 0 ? actual_rep : 0);
                lz_advance_skip(&lz, nd->len);
                emit_pos += nd->len;
            }
        }
        free(path_pos);
    }

    size_t r = ac_enc_flush(&enc);
    free(opt); free(mcache); free(mcache_count); free(lit_costs);
    free(o1_freq); free(o2_freq);
    lz_free(&lz); cm_free(&cm); if (use_cm_pricing) cm_free(&price_cm); free(lm);
    return r;
}
static size_t decompress_m1(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size) {
    cm_state_t cm; if (cm_init(&cm) != 0) return 0;
    lz_model_t *lm = calloc(1, sizeof(lz_model_t));
    if (!lm) { cm_free(&cm); return 0; }
    lz_model_init(lm);

    ac_decoder_t dec; ac_dec_init(&dec, in, in_size);
    uint32_t reps[LZ_NUM_REPS] = {1, 1, 1, 1};
    size_t pos = 0;
    uint32_t last_match_dist = 0;

    while (pos < out_size) {
        uint32_t ps = pos & 3;

        if (!ab_dec(&lm->is_match[ps], &dec)) {
            /* Literal */
            if (last_match_dist > 0 && pos >= last_match_dist) {
                cm.lz_match_byte = out[pos - last_match_dist];
                cm.lz_match_active = 1;
            } else {
                cm.lz_match_active = 0;
            }
            out[pos] = cm_decode_byte(&cm, &dec);
            pos++;
        } else {
            /* Match */
            uint32_t dist, len;
            int is_rep = ab_dec(&lm->is_rep, &dec);

            if (is_rep) {
                int idx = 0;
                if (ab_dec(&lm->rep_idx[0], &dec)) {
                    idx = 1;
                    if (ab_dec(&lm->rep_idx[1], &dec)) {
                        idx = 2;
                        if (ab_dec(&lm->rep_idx[2], &dec)) idx = 3;
                    }
                }
                dist = reps[idx];
                /* Move to front */
                for (int i = idx; i > 0; i--) reps[i] = reps[i-1];
                reps[0] = dist;
                /* Decode rep-match length */
                len = bt_dec(lm->rep_len_tree, 9, &dec) + LZ_MIN_MATCH;
            } else {
                /* Decode length first (for distance context) */
                len = bt_dec(lm->len_tree, 9, &dec) + LZ_MIN_MATCH;

                /* Decode distance */
                uint32_t slot = bt_dec(lm->dist_slot, 6, &dec);
                uint32_t extra = slot_extra_bits(slot);
                if (extra == 0) {
                    dist = slot_to_dist_base(slot);
                } else {
                    uint32_t base = slot_to_dist_base(slot);
                    uint32_t rem = 0;
                    if (extra <= 4) {
                        /* Small distances: fully adaptive */
                        uint32_t ctx_base = (slot - 4) * 16;
                        uint32_t tree_ctx = 1;
                        for (uint32_t i = 0; i < extra; i++) {
                            int bit = ab_dec(&lm->dist_special[ctx_base + tree_ctx], &dec);
                            tree_ctx = (tree_ctx << 1) | bit;
                            rem = (rem << 1) | bit;
                        }
                    } else {
                        /* Large distances: high bits direct, low 4 adaptive */
                        uint32_t high_bits = extra - 4;
                        uint32_t high_val = 0;
                        for (uint32_t i = 0; i < high_bits; i++)
                            high_val = (high_val << 1) | ac_dec_bit(&dec, PROB_HALF);
                        uint32_t low4 = 0;
                        uint32_t tree_ctx = 1;
                        for (int i = 0; i < 4; i++) {
                            int bit = ab_dec(&lm->dist_align[tree_ctx], &dec);
                            tree_ctx = (tree_ctx << 1) | bit;
                            low4 = (low4 << 1) | bit;
                        }
                        rem = (high_val << 4) | low4;
                    }
                    dist = base + rem;
                }
                /* Update reps */
                for (int i = LZ_NUM_REPS-1; i > 0; i--) reps[i] = reps[i-1];
                reps[0] = dist;
            }

            /* Copy and sync */
            if (dist == 0 || dist > pos) { cm_free(&cm); free(lm); return 0; }
            {
                /* Defer stride detection during match (must match encoder) */
                predictor_stride_t *st = cm.stride;
                size_t saved_next = st->next_detect;
                st->next_detect = st->pos + len + STRIDE_DETECT_INTERVAL;
                for (uint32_t j = 0; j < len && pos < out_size; j++) {
                    out[pos] = out[pos - dist];
                    uint8_t byte = out[pos];
                    pred_match_update_byte(&cm.match, byte);
                    pred_rl_update_byte(&cm.rl, byte);
                    st->buf[st->pos & (STRIDE_BUF_SIZE - 1)] = byte;
                    st->pos++;
                    cm.history[0] = cm.history[1]; cm.history[1] = cm.history[2];
                    cm.history[2] = cm.history[3]; cm.history[3] = cm.history[4];
                    cm.history[4] = cm.history[5]; cm.history[5] = cm.history[6];
                    cm.history[6] = cm.history[7]; cm.history[7] = byte;
                    cm.byte_pos++;
                    pos++;
                }
                if (st->pos >= saved_next) {
                    st->next_detect = st->pos;
                } else {
                    st->next_detect = saved_next;
                }
            }
            last_match_dist = dist;
        }
    }

    cm_free(&cm); free(lm);
    return out_size;
}

/* --- Public API: tries both methods, picks smaller --- */
/* Method byte: 0=CM, 1=LZ+CM, 2=BCJ+CM, 3=BCJ+LZ+CM */
/* Detect best stride for column-split transpose.
 * Returns 0 if no good stride found, otherwise the stride value.
 * Only triggers if a MAJORITY of the data benefits from the stride. */
static int detect_stride(const uint8_t *data, size_t size) {
    if (size < 4096) return 0;
    static const int candidates[] = {4, 8, 12, 16, 20, 24, 32};
    int best_stride = 0;
    int best_score = 0;
    /* Check the whole block (sampled) */
    size_t step = size > 65536 ? size / 65536 : 1;

    for (int ci = 0; ci < 7; ci++) {
        int s = candidates[ci];
        if ((size_t)s >= size) continue;
        int matches = 0, total = 0;
        for (size_t i = s; i < size; i += step) {
            matches += (data[i] == data[i - s]);
            total++;
        }
        /* Need >55% autocorrelation across the whole block */
        if (total > 0 && matches * 100 > total * 55 && matches > best_score) {
            best_score = matches;
            best_stride = s;
        }
    }
    return best_stride;
}

/* Transpose data with given stride (column-split) */
static void transpose(const uint8_t *in, uint8_t *out, size_t size, int stride) {
    size_t n_full = size / stride;
    size_t rem = size % stride;
    size_t pos = 0;
    for (int col = 0; col < stride; col++) {
        for (size_t row = 0; row < n_full; row++)
            out[pos++] = in[row * stride + col];
    }
    for (size_t i = 0; i < rem; i++)
        out[pos++] = in[n_full * stride + i];
}

/* Reverse transpose */
static void untranspose(const uint8_t *in, uint8_t *out, size_t size, int stride) {
    size_t n_full = size / stride;
    size_t rem = size % stride;
    size_t pos = 0;
    for (int col = 0; col < stride; col++) {
        for (size_t row = 0; row < n_full; row++)
            out[row * stride + col] = in[pos++];
    }
    for (size_t i = 0; i < rem; i++)
        out[n_full * stride + i] = in[pos++];
}

/* Parallel section compression support */
typedef struct {
    const uint8_t *src;
    uint32_t src_size;
    uint8_t *out;
    size_t out_size;
    size_t result;
    int use_m1;  /* 1 = compress_m1 (code section), 0 = krv_compress_block */
} par_sec_t;

static void *par_sec_worker(void *arg) {
    par_sec_t *p = (par_sec_t *)arg;
    if (p->use_m1)
        p->result = compress_m1(p->src, p->src_size, p->out, p->out_size);
    else
        p->result = krv_compress_block(p->src, p->src_size, p->out, p->out_size);
    return NULL;
}

static __thread int compress_depth = 0;

size_t krv_compress_block(const uint8_t *in, size_t in_size,
                          uint8_t *out, size_t out_size) {
    if (in_size == 0) return 0;
    if (out_size < 2) return 0;
    compress_depth++;

    size_t avail = out_size - 1;
    size_t best = avail + 1;
    uint8_t best_method = 0;
    uint8_t *best_buf = malloc(avail);
    if (!best_buf) { compress_depth--; return 0; }

    /* Try method 0: pure CM (only for small data where LZ overhead hurts) */
    size_t s;
    if (in_size <= 65536) {
        s = compress_m0(in, in_size, best_buf, avail);
        if (s > 0 && s < best) { best = s; best_method = 0; }
    }

    /* Detect ELF early to skip redundant trials */
    int is_elf = (compress_depth == 1 && in_size >= 64 &&
                  in[0]==0x7F && in[1]=='E' && in[2]=='L' && in[3]=='F');

    /* Try method 1: LZ+CM (skip for large ELF — section splits always win) */
    uint8_t *tmp = malloc(avail);
    if (tmp) {
        if (!(is_elf && in_size > 65536)) {
            s = compress_m1(in, in_size, tmp, avail);
            if (s > 0 && s < best) { best = s; best_method = 1; memcpy(best_buf, tmp, s); }
        }

        /* Try BCJ methods if data looks like x86 code */
        if (compress_depth <= 1 && bcj_detect_x86(in, in_size)) {
            uint8_t *bcj_data = malloc(in_size);
            if (bcj_data) {
                memcpy(bcj_data, in, in_size);
                bcj_filter(bcj_data, in_size, 0);

                /* Method 2: BCJ + CM (only small data where LZ overhead hurts) */
                if (in_size <= 65536) {
                    s = compress_m0(bcj_data, in_size, tmp, avail);
                    if (s > 0 && s < best) { best = s; best_method = 2; memcpy(best_buf, tmp, s); }
                }

                /* Method 3: BCJ + LZ+CM (skip if ELF-BCJ available — it's always better) */
                if (!is_elf) {
                    s = compress_m1(bcj_data, in_size, tmp, avail);
                    if (s > 0 && s < best) { best = s; best_method = 3; memcpy(best_buf, tmp, s); }
                }

                free(bcj_data);
            }

            /* Method 6/7: ELF-aware BCJ — only at top level (sub-blocks aren't ELF) */
            if (compress_depth == 1) {
            bcj_data = malloc(in_size);
            if (bcj_data) {
                memcpy(bcj_data, in, in_size);
                bcj_filter_elf(bcj_data, in_size);

                /* Method 6: ELF-BCJ + CM (only small data) */
                if (in_size <= 65536) {
                    s = compress_m0(bcj_data, in_size, tmp, avail);
                    if (s > 0 && s < best) { best = s; best_method = 6; memcpy(best_buf, tmp, s); }
                }

                /* Method 7: ELF-BCJ + LZ+CM (skip for large ELF — method 10 always wins) */
                if (!(is_elf && in_size > 262144)) {
                    s = compress_m1(bcj_data, in_size, tmp, avail);
                    if (s > 0 && s < best) { best = s; best_method = 7; memcpy(best_buf, tmp, s); }
                }

                free(bcj_data);
            }
            }
        }
        free(tmp);
    }

    /* Try stride-transpose if data has regular structure */
    int stride = detect_stride(in, in_size);
    if (stride > 0) {
        uint8_t *t_data = malloc(in_size);
        uint8_t *t_out = malloc(avail);
        if (t_data && t_out) {
            transpose(in, t_data, in_size, stride);
            /* Method 4: stride + LZ+CM */
            s = compress_m1(t_data, in_size, t_out, avail);
            if (s > 0 && s + 1 < best) {
                best = s + 1; /* +1 for stride byte */
                best_method = 4;
                best_buf[0] = (uint8_t)stride;
                memcpy(best_buf + 1, t_out, s);
            }
            /* Method 5: stride + BCJ + LZ+CM (for code with stride patterns) */
            if (bcj_detect_x86(t_data, in_size)) {
                bcj_filter(t_data, in_size, 0);
                s = compress_m1(t_data, in_size, t_out, avail);
                if (s > 0 && s + 1 < best) {
                    best = s + 1;
                    best_method = 5;
                    best_buf[0] = (uint8_t)stride;
                    memcpy(best_buf + 1, t_out, s);
                }
            }
        }
        free(t_data); free(t_out);
    }

    /* Method 12: .eh_frame stream separation (DWARF CFI) */
    if (in_size >= 24) {
        uint32_t first_len, first_cie;
        memcpy(&first_len, in, 4);
        memcpy(&first_cie, in + 4, 4);
        if (first_cie == 0 && first_len >= 4 && first_len < in_size &&
            in[8] == 1 && in[9] == 'z') {
            size_t pos12 = 0;
            int nrec = 0, valid = 1;
            while (pos12 < in_size - 4) {
                uint32_t rlen; memcpy(&rlen, in + pos12, 4);
                if (rlen == 0) break;
                if (pos12 + 4 + rlen > in_size) { valid = 0; break; }
                nrec++; pos12 += 4 + rlen;
            }
            if (valid && nrec >= 8) {
                uint8_t *flags = calloc((nrec + 7) / 8, 1);
                uint8_t *lengths = malloc(nrec * 2);
                uint8_t *pc_begins = malloc(nrec * 4);
                uint8_t *pc_ranges = malloc(nrec * 4);
                uint8_t *cie_refs = malloc(nrec);
                uint8_t *bodies = malloc(in_size);
                if (flags && lengths && pc_begins && pc_ranges && cie_refs && bodies) {
                    int n_fde = 0, n_cie = 0;
                    size_t body_pos = 0;
                    int32_t prev_pc = 0;
                    uint32_t cie_positions[16];
                    pos12 = 0;
                    for (int ri = 0; ri < nrec; ri++) {
                        uint32_t rlen; memcpy(&rlen, in + pos12, 4);
                        uint16_t l16 = (uint16_t)(rlen > 65535 ? 65535 : rlen);
                        memcpy(lengths + ri * 2, &l16, 2);
                        uint32_t cie_ptr; memcpy(&cie_ptr, in + pos12 + 4, 4);
                        if (cie_ptr == 0) {
                            if (n_cie < 16) cie_positions[n_cie++] = (uint32_t)pos12;
                            memcpy(bodies + body_pos, in + pos12 + 4, rlen);
                            body_pos += rlen;
                        } else {
                            flags[ri / 8] |= (1 << (ri & 7));
                            uint32_t actual_cie = (uint32_t)(pos12 + 4) - cie_ptr;
                            uint8_t cidx = 0;
                            for (int ci = 0; ci < n_cie; ci++)
                                if (cie_positions[ci] == actual_cie) { cidx = (uint8_t)ci; break; }
                            cie_refs[n_fde] = cidx;
                            int32_t pc_begin; memcpy(&pc_begin, in + pos12 + 8, 4);
                            uint32_t pc_range; memcpy(&pc_range, in + pos12 + 12, 4);
                            int32_t delta = pc_begin - prev_pc; prev_pc = pc_begin;
                            memcpy(pc_begins + n_fde * 4, &delta, 4);
                            memcpy(pc_ranges + n_fde * 4, &pc_range, 4);
                            n_fde++;
                            if (rlen > 12) {
                                memcpy(bodies + body_pos, in + pos12 + 16, rlen - 12);
                                body_pos += rlen - 12;
                            }
                        }
                        pos12 += 4 + rlen;
                    }
                    /* hdr: 4(nrec)+4(n_fde)+4(flags_sz)+4(cl)+4(cpb)+4(cpr)+4(cr)+4(cb)=32 */
                    uint32_t flags_sz32 = (uint32_t)((nrec + 7) / 8);
                    size_t hdr12 = 32;
                    uint8_t *out12 = malloc(avail);
                    if (out12 && avail > hdr12) {
                        size_t p12 = hdr12;
                        memcpy(out12 + p12, flags, flags_sz32); p12 += flags_sz32;
                        size_t cl = compress_m1(lengths, (size_t)nrec*2, out12+p12, avail-p12);
                        if (cl > 0) { p12 += cl;
                        size_t cpb = compress_m1(pc_begins, (size_t)n_fde*4, out12+p12, avail-p12);
                        if (cpb > 0) { p12 += cpb;
                        size_t cpr = compress_m1(pc_ranges, (size_t)n_fde*4, out12+p12, avail-p12);
                        if (cpr > 0) { p12 += cpr;
                        size_t cr = compress_m1(cie_refs, (size_t)n_fde, out12+p12, avail-p12);
                        if (cr > 0) { p12 += cr;
                        size_t cb = compress_m1(bodies, body_pos, out12+p12, avail-p12);
                        if (cb > 0) { p12 += cb;
                        if (p12 < best) {
                            uint32_t nr32=(uint32_t)nrec, nf32=(uint32_t)n_fde;
                            uint32_t cl32=(uint32_t)cl, cpb32=(uint32_t)cpb;
                            uint32_t cpr32=(uint32_t)cpr, cr32=(uint32_t)cr, cb32=(uint32_t)cb;
                            memcpy(out12,&nr32,4); memcpy(out12+4,&nf32,4);
                            memcpy(out12+8,&flags_sz32,4); memcpy(out12+12,&cl32,4);
                            memcpy(out12+16,&cpb32,4); memcpy(out12+20,&cpr32,4);
                            memcpy(out12+24,&cr32,4); memcpy(out12+28,&cb32,4);
                            best = p12; best_method = 12;
                            memcpy(best_buf, out12, best);
                        }
                        }}}}}
                    }
                    free(out12);
                }
                free(flags); free(lengths); free(pc_begins); free(pc_ranges); free(cie_refs); free(bodies);
            }
        }
    }

    /* Method 13: content-type separation (string vs binary blocks) */
    if (in_size >= 4096 && compress_depth <= 2) {
        uint32_t nblk = (uint32_t)(in_size / 64);
        uint32_t str_count = 0;
        for (uint32_t bi = 0; bi < nblk; bi++) {
            const uint8_t *blk = in + (size_t)bi * 64;
            int printable = 0;
            for (int j = 0; j < 64; j++)
                if ((blk[j] >= 32 && blk[j] <= 126) || blk[j] == 9 || blk[j] == 10 || blk[j] == 13 || blk[j] == 0)
                    printable++;
            if (printable > 56) str_count++;
        }
        uint32_t bin_count = nblk - str_count;
        /* Only split if both streams are substantial */
        if (str_count >= nblk / 5 && bin_count >= nblk / 5 && nblk >= 16) {
            uint32_t bitmap_sz = (nblk + 7) / 8;
            uint8_t *bitmap = calloc(bitmap_sz, 1);
            uint32_t str_size = 0, bin_size = 0;
            if (bitmap) {
                for (uint32_t bi = 0; bi < nblk; bi++) {
                    const uint8_t *blk = in + (size_t)bi * 64;
                    int printable = 0;
                    for (int j = 0; j < 64; j++)
                        if ((blk[j] >= 32 && blk[j] <= 126) || blk[j] == 9 || blk[j] == 10 || blk[j] == 13 || blk[j] == 0)
                            printable++;
                    if (printable > 56) { bitmap[bi/8] |= (1<<(bi&7)); str_size += 64; }
                    else bin_size += 64;
                }
                uint32_t tail = (uint32_t)(in_size - (size_t)nblk * 64);
                bin_size += tail; /* tail goes to binary stream */
                uint8_t *str_buf = malloc(str_size);
                uint8_t *bin_buf = malloc(bin_size);
                uint8_t *out13 = malloc(avail);
                if (str_buf && bin_buf && out13) {
                    uint32_t sp = 0, bp = 0;
                    for (uint32_t bi = 0; bi < nblk; bi++) {
                        if ((bitmap[bi/8] >> (bi&7)) & 1)
                            { memcpy(str_buf + sp, in + (size_t)bi*64, 64); sp += 64; }
                        else
                            { memcpy(bin_buf + bp, in + (size_t)bi*64, 64); bp += 64; }
                    }
                    if (tail) { memcpy(bin_buf + bp, in + (size_t)nblk*64, tail); bp += tail; }
                    /* Header: 4(nblk) + 4(str_comp_size) + bitmap */
                    size_t hdr13 = 8 + bitmap_sz;
                    if (avail > hdr13) {
                        size_t p13 = hdr13;
                        size_t cs = krv_compress_block(str_buf, str_size, out13 + p13, avail - p13);
                        if (cs > 0) { p13 += cs;
                        size_t cb = krv_compress_block(bin_buf, bin_size, out13 + p13, avail - p13);
                        if (cb > 0) { p13 += cb;
                        if (p13 < best) {
                            uint32_t cs32 = (uint32_t)cs;
                            memcpy(out13, &nblk, 4);
                            memcpy(out13 + 4, &cs32, 4);
                            memcpy(out13 + 8, bitmap, bitmap_sz);
                            best = p13; best_method = 13;
                            memcpy(best_buf, out13, best);
                        }
                        }}
                    }
                }
                free(str_buf); free(bin_buf); free(out13); free(bitmap);
            }
        }
    }

    /* Method 8: ELF section split — compress code and non-code separately */
    if (compress_depth == 1 && in_size >= 64 && in[0]==0x7F && in[1]=='E' && in[2]=='L' && in[3]=='F') {
        int is64 = (in[4] == 2);
        uint64_t sh_off; uint16_t sh_ent, sh_num;
        if (is64 && in_size >= 64) {
            memcpy(&sh_off, in + 40, 8);
            memcpy(&sh_ent, in + 58, 2);
            memcpy(&sh_num, in + 60, 2);
        } else if (in_size >= 52) {
            uint32_t o32; memcpy(&o32, in + 32, 4); sh_off = o32;
            memcpy(&sh_ent, in + 46, 2);
            memcpy(&sh_num, in + 48, 2);
        } else { sh_off = 0; sh_num = 0; sh_ent = 0; }

        if (sh_off && sh_num && sh_off + (uint64_t)sh_ent * sh_num <= in_size) {
            /* Find contiguous code region */
            uint32_t code_off = 0, code_end = 0;
            for (uint16_t i = 0; i < sh_num; i++) {
                const uint8_t *sh = in + sh_off + (uint64_t)i * sh_ent;
                uint64_t s_flags, s_offset, s_size;
                if (is64) {
                    memcpy(&s_flags, sh + 8, 8);
                    memcpy(&s_offset, sh + 24, 8);
                    memcpy(&s_size, sh + 32, 8);
                } else {
                    uint32_t f32, o32, sz32;
                    memcpy(&f32, sh + 8, 4); memcpy(&o32, sh + 16, 4); memcpy(&sz32, sh + 20, 4);
                    s_flags = f32; s_offset = o32; s_size = sz32;
                }
                if ((s_flags & 0x4) && s_offset + s_size <= in_size && s_size > 0) {
                    uint32_t so = (uint32_t)s_offset, se = (uint32_t)(s_offset + s_size);
                    if (code_off == 0 || so < code_off) code_off = so;
                    if (se > code_end) code_end = se;
                }
            }
            uint32_t code_size = code_end > code_off ? code_end - code_off : 0;
            uint32_t rest_size = (uint32_t)in_size - code_size;

            if (code_size >= 1024 && rest_size >= 64) {
                uint8_t *seg_out = malloc(avail);
                if (seg_out) {
                    /* Method 8: split into separate streams */
                    uint8_t *code_buf = malloc(code_size);
                    uint8_t *rest_buf = malloc(rest_size);
                    if (code_buf && rest_buf) {
                        memcpy(code_buf, in + code_off, code_size);
                        bcj_filter(code_buf, code_size, code_off);
                        memcpy(rest_buf, in, code_off);
                        memcpy(rest_buf + code_off, in + code_end, in_size - code_end);

                        size_t hdr = 12;
                        if (avail > hdr) {
                            size_t sc = compress_m1(code_buf, code_size, seg_out + hdr, avail - hdr);
                            if (sc > 0) {
                                /* Use full block compression for rest (tries stride, etc.) */
                                size_t sr = krv_compress_block(rest_buf, rest_size, seg_out + hdr + sc, avail - hdr - sc);
                                if (sr > 0 && hdr + sc + sr < best) {
                                    memcpy(seg_out, &code_off, 4);
                                    memcpy(seg_out + 4, &code_size, 4);
                                    uint32_t sc32 = (uint32_t)sc;
                                    memcpy(seg_out + 8, &sc32, 4);
                                    best = hdr + sc + sr;
                                    best_method = 8;
                                    memcpy(best_buf, seg_out, best);
                                }
                            }
                        }
                    }
                    free(code_buf); free(rest_buf);

                    /* Method 10: per-section split — each non-code section compressed individually */
                    {
                        /* Collect non-code sections (skip NOBITS, skip tiny) */
                        typedef struct { uint32_t off, size; } sec_t;
                        sec_t secs[64];
                        int nsecs = 0;
                        uint8_t *covered = calloc(in_size, 1); /* track covered bytes */
                        if (covered) {
                            /* Mark code region as covered */
                            memset(covered + code_off, 1, code_size);
                            for (uint16_t i = 0; i < sh_num && nsecs < 62; i++) {
                                const uint8_t *sh = in + sh_off + (uint64_t)i * sh_ent;
                                uint64_t s_flags, s_offset, s_size;
                                uint32_t s_type;
                                if (is64) {
                                    memcpy(&s_type, sh + 4, 4);
                                    memcpy(&s_flags, sh + 8, 8);
                                    memcpy(&s_offset, sh + 24, 8);
                                    memcpy(&s_size, sh + 32, 8);
                                } else {
                                    memcpy(&s_type, sh + 4, 4);
                                    uint32_t f32, o32, sz32;
                                    memcpy(&f32, sh + 8, 4); memcpy(&o32, sh + 16, 4); memcpy(&sz32, sh + 20, 4);
                                    s_flags = f32; s_offset = o32; s_size = sz32;
                                }
                                if ((s_flags & 0x4) || s_type == 8 || s_size < 4096) continue;
                                if (s_offset + s_size > in_size) continue;
                                secs[nsecs].off = (uint32_t)s_offset;
                                secs[nsecs].size = (uint32_t)s_size;
                                memset(covered + s_offset, 1, s_size);
                                nsecs++;
                            }
                            /* Build gaps buffer (uncovered bytes) */
                            uint32_t gap_size = 0;
                            for (size_t gi = 0; gi < in_size; gi++)
                                if (!covered[gi]) gap_size++;
                            if (gap_size > 0 && nsecs < 63) {
                                /* Add gaps as last section — store at a special offset */
                                /* We'll build the gap buffer and use offset=0xFFFFFFFF as marker */
                                secs[nsecs].off = 0xFFFFFFFF; /* marker for gaps */
                                secs[nsecs].size = gap_size;
                                nsecs++;
                            }
                            if (nsecs >= 2) {
                                size_t hdr10 = 13 + (size_t)nsecs * 12;
                                uint8_t *code_buf2 = malloc(code_size);
                                uint8_t *gap_buf = gap_size > 0 ? malloc(gap_size) : NULL;
                                if (gap_size > 0 && gap_buf) {
                                    uint32_t gp = 0;
                                    for (size_t gi = 0; gi < in_size; gi++)
                                        if (!covered[gi]) gap_buf[gp++] = in[gi];
                                }
                                if (code_buf2 && avail > hdr10) {
                                    memcpy(code_buf2, in + code_off, code_size);
                                    bcj_filter(code_buf2, code_size, code_off);

                                    /* Compress code section + all data sections in parallel */
                                    int ok = 1;
                                    uint32_t comp_sizes[64];

                                    /* Code section as par[nsecs] */
                                    par_sec_t par[65];
                                    pthread_t threads[65];
                                    int n_jobs = nsecs + 1;

                                    /* Set up code section job */
                                    par[nsecs].src = code_buf2;
                                    par[nsecs].src_size = code_size;
                                    par[nsecs].out_size = code_size + 4096;
                                    par[nsecs].out = malloc(par[nsecs].out_size);
                                    par[nsecs].result = 0;
                                    par[nsecs].use_m1 = 1;
                                    if (!par[nsecs].out) ok = 0;

                                    /* Set up data section jobs */
                                    for (int si = 0; si < nsecs && ok; si++) {
                                        if (secs[si].off == 0xFFFFFFFF) {
                                            par[si].src = gap_buf;
                                            par[si].src_size = secs[si].size;
                                        } else {
                                            par[si].src = in + secs[si].off;
                                            par[si].src_size = secs[si].size;
                                        }
                                        par[si].out_size = par[si].src_size + 4096;
                                        par[si].out = malloc(par[si].out_size);
                                        par[si].result = 0;
                                        par[si].use_m1 = 0;
                                        if (!par[si].out) { ok = 0; break; }
                                    }

                                    if (ok) {
                                        /* Launch all jobs in parallel */
                                        int launched = 0;
                                        for (int si = 0; si < n_jobs; si++) {
                                            if (pthread_create(&threads[si], NULL, par_sec_worker, &par[si]) != 0) {
                                                par_sec_worker(&par[si]);
                                            } else {
                                                launched = si + 1;
                                            }
                                        }
                                        for (int si = 0; si < launched; si++)
                                            pthread_join(threads[si], NULL);

                                        /* Check code section result */
                                        size_t sc2 = par[nsecs].result;
                                        if (sc2 == 0) ok = 0;

                                        /* Assemble output: header + code + sections */
                                        size_t pos10 = hdr10;
                                        if (ok) {
                                            memcpy(seg_out + pos10, par[nsecs].out, sc2);
                                            pos10 += sc2;
                                        }
                                        for (int si = 0; si < nsecs && ok; si++) {
                                            if (par[si].result == 0 || pos10 + par[si].result > avail) {
                                                ok = 0; break;
                                            }
                                            memcpy(seg_out + pos10, par[si].out, par[si].result);
                                            comp_sizes[si] = (uint32_t)par[si].result;
                                            pos10 += par[si].result;
                                        }

                                        if (ok && pos10 < best) {
                                            seg_out[0] = (uint8_t)nsecs;
                                            memcpy(seg_out + 1, &code_off, 4);
                                            memcpy(seg_out + 5, &code_size, 4);
                                            uint32_t sc2_32 = (uint32_t)sc2;
                                            memcpy(seg_out + 9, &sc2_32, 4);
                                            for (int si = 0; si < nsecs; si++) {
                                                size_t base = 13 + (size_t)si * 12;
                                                memcpy(seg_out + base, &secs[si].off, 4);
                                                memcpy(seg_out + base + 4, &secs[si].size, 4);
                                                memcpy(seg_out + base + 8, &comp_sizes[si], 4);
                                            }
                                            best = pos10;
                                            best_method = 10;
                                            memcpy(best_buf, seg_out, best);
                                        }
                                    }

                                    /* Free all buffers */
                                    for (int si = 0; si < n_jobs; si++)
                                        free(par[si].out);
                                }
                                free(code_buf2); free(gap_buf);
                            }
                            free(covered);
                        }
                    }

                    free(seg_out);
                }

                /* Method 11: Hybrid split — code with BCJ separate, stride sections separate,
                 * all remaining non-code sections combined into one stream for cross-section LZ
                 * (only helps small binaries — method 10 always wins for large ELF) */
                if (in_size <= 262144) {
                    uint8_t *seg_out11 = malloc(avail);
                    if (seg_out11) {
                        /* Identify stride sections among non-code */
                        typedef struct { uint32_t off, size; } sec11_t;
                        sec11_t stride_secs[16];
                        int n_stride = 0;

                        for (uint16_t i = 0; i < sh_num && n_stride < 16; i++) {
                            const uint8_t *sh = in + sh_off + (uint64_t)i * sh_ent;
                            uint64_t s_flags, s_offset, s_size;
                            uint32_t s_type;
                            if (is64) {
                                memcpy(&s_type, sh + 4, 4);
                                memcpy(&s_flags, sh + 8, 8);
                                memcpy(&s_offset, sh + 24, 8);
                                memcpy(&s_size, sh + 32, 8);
                            } else {
                                memcpy(&s_type, sh + 4, 4);
                                uint32_t f32, o32, sz32;
                                memcpy(&f32, sh + 8, 4); memcpy(&o32, sh + 16, 4); memcpy(&sz32, sh + 20, 4);
                                s_flags = f32; s_offset = o32; s_size = sz32;
                            }
                            if ((s_flags & 0x4) || s_type == 8 || s_size < 4096) continue;
                            if (s_offset + s_size > in_size) continue;
                            /* Check if this section has stride */
                            if (detect_stride(in + (uint32_t)s_offset, (size_t)s_size) > 0) {
                                stride_secs[n_stride].off = (uint32_t)s_offset;
                                stride_secs[n_stride].size = (uint32_t)s_size;
                                n_stride++;
                            }
                        }

                        /* Build combined "rest" buffer: everything except code and stride sections */
                        uint32_t rest_size11 = (uint32_t)in_size - code_size;
                        for (int si = 0; si < n_stride; si++)
                            rest_size11 -= stride_secs[si].size;

                        uint8_t *rest_buf11 = malloc(rest_size11);
                        if (rest_buf11 && rest_size11 > 0) {
                            uint32_t rp = 0;
                            for (size_t gi = 0; gi < in_size; gi++) {
                                /* Skip code region */
                                if (gi >= code_off && gi < code_end) continue;
                                /* Skip stride sections */
                                int skip = 0;
                                for (int si = 0; si < n_stride; si++)
                                    if (gi >= stride_secs[si].off && gi < stride_secs[si].off + stride_secs[si].size)
                                        { skip = 1; break; }
                                if (skip) continue;
                                rest_buf11[rp++] = in[gi];
                            }
                            rest_size11 = rp;

                            /* Header: 1(n_stride) + 4(code_off) + 4(code_size) + 4(comp_code) + 4(comp_rest)
                             *        + n_stride * 12 (off, size, comp_size) */
                            size_t hdr11 = 17 + (size_t)n_stride * 12;
                            if (avail > hdr11) {
                                /* Compress code with BCJ */
                                uint8_t *code_buf11 = malloc(code_size);
                                if (code_buf11) {
                                    memcpy(code_buf11, in + code_off, code_size);
                                    bcj_filter(code_buf11, code_size, code_off);
                                    size_t pos11 = hdr11;
                                    size_t sc11 = compress_m1(code_buf11, code_size, seg_out11 + pos11, avail - pos11);
                                    if (sc11 > 0) {
                                        pos11 += sc11;
                                        /* Compress stride sections individually */
                                        int ok11 = 1;
                                        uint32_t stride_comp[16];
                                        for (int si = 0; si < n_stride && ok11; si++) {
                                            size_t ss = krv_compress_block(in + stride_secs[si].off, stride_secs[si].size,
                                                                           seg_out11 + pos11, avail - pos11);
                                            if (ss == 0) { ok11 = 0; break; }
                                            stride_comp[si] = (uint32_t)ss;
                                            pos11 += ss;
                                        }
                                        /* Compress combined rest as one stream */
                                        size_t sr11 = 0;
                                        if (ok11 && rest_size11 > 0) {
                                            sr11 = krv_compress_block(rest_buf11, rest_size11,
                                                                      seg_out11 + pos11, avail - pos11);
                                            if (sr11 == 0) ok11 = 0;
                                            else pos11 += sr11;
                                        }
                                        if (ok11 && pos11 < best) {
                                            seg_out11[0] = (uint8_t)n_stride;
                                            memcpy(seg_out11 + 1, &code_off, 4);
                                            memcpy(seg_out11 + 5, &code_size, 4);
                                            uint32_t sc11_32 = (uint32_t)sc11;
                                            memcpy(seg_out11 + 9, &sc11_32, 4);
                                            uint32_t sr11_32 = (uint32_t)sr11;
                                            memcpy(seg_out11 + 13, &sr11_32, 4);
                                            for (int si = 0; si < n_stride; si++) {
                                                size_t base11 = 17 + (size_t)si * 12;
                                                memcpy(seg_out11 + base11, &stride_secs[si].off, 4);
                                                memcpy(seg_out11 + base11 + 4, &stride_secs[si].size, 4);
                                                memcpy(seg_out11 + base11 + 8, &stride_comp[si], 4);
                                            }
                                            best = pos11;
                                            best_method = 11;
                                            memcpy(best_buf, seg_out11, best);
                                        }
                                    }
                                    free(code_buf11);
                                }
                            }
                        }
                        free(rest_buf11);
                        free(seg_out11);
                    }
                }
            }
        }
    }

    out[0] = best_method;
    memcpy(out + 1, best_buf, best);
    free(best_buf);
    compress_depth--;
    return best + 1;
}

size_t krv_decompress_block(const uint8_t *in, size_t in_size,
                            uint8_t *out, size_t out_size) {
    if (in_size < 1 || out_size == 0) return 0;
    uint8_t method = in[0];
    size_t r = 0;

    if (method == 8) {
        /* ELF section split: code + rest compressed separately */
        if (in_size < 14) return 0;
        uint32_t code_off, code_size, comp_code_size;
        memcpy(&code_off, in + 1, 4);
        memcpy(&code_size, in + 5, 4);
        memcpy(&comp_code_size, in + 9, 4);
        uint32_t rest_size = (uint32_t)out_size - code_size;
        if (code_off + code_size > out_size) return 0;
        if (13 + comp_code_size > in_size) return 0;

        uint8_t *code_buf = malloc(code_size);
        if (!code_buf) return 0;
        size_t rc = decompress_m1(in + 13, comp_code_size, code_buf, code_size);
        if (rc != code_size) { free(code_buf); return 0; }
        bcj_unfilter(code_buf, code_size, code_off);

        uint8_t *rest_buf = malloc(rest_size);
        if (!rest_buf) { free(code_buf); return 0; }
        size_t rr = krv_decompress_block(in + 13 + comp_code_size, in_size - 13 - comp_code_size, rest_buf, rest_size);
        if (rr != rest_size) { free(code_buf); free(rest_buf); return 0; }

        memcpy(out, rest_buf, code_off);
        memcpy(out + code_off, code_buf, code_size);
        memcpy(out + code_off + code_size, rest_buf + code_off, rest_size - code_off);

        free(code_buf); free(rest_buf);
        return out_size;
    } else if (method == 10) {
        /* ELF per-section split */
        if (in_size < 14) return 0;
        uint8_t nsecs = in[1];
        uint32_t code_off, code_size, comp_code_size;
        memcpy(&code_off, in + 2, 4);
        memcpy(&code_size, in + 6, 4);
        memcpy(&comp_code_size, in + 10, 4);
        size_t hdr10 = 14 + (size_t)nsecs * 12;
        if (in_size < hdr10 + comp_code_size) return 0;

        /* Decompress code */
        uint8_t *code_buf = malloc(code_size);
        if (!code_buf) return 0;
        size_t rc = decompress_m1(in + hdr10, comp_code_size, code_buf, code_size);
        if (rc != code_size) { free(code_buf); return 0; }
        bcj_unfilter(code_buf, code_size, code_off);

        memset(out, 0, out_size);
        memcpy(out + code_off, code_buf, code_size);
        free(code_buf);

        /* Track coverage for gap reconstruction */
        uint8_t *covered = calloc(out_size, 1);
        if (!covered) return 0;
        memset(covered + code_off, 1, code_size);

        /* Decompress each section */
        size_t pos10 = hdr10 + comp_code_size;
        uint8_t *gap_data = NULL;
        for (int si = 0; si < nsecs; si++) {
            size_t base = 14 + (size_t)si * 12;
            uint32_t s_off, s_size, s_comp;
            memcpy(&s_off, in + base, 4);
            memcpy(&s_size, in + base + 4, 4);
            memcpy(&s_comp, in + base + 8, 4);
            if (pos10 + s_comp > in_size) { free(covered); free(gap_data); return 0; }
            if (s_off == 0xFFFFFFFF) {
                /* Gaps segment */
                gap_data = malloc(s_size);
                if (!gap_data) { free(covered); return 0; }
                size_t rs = krv_decompress_block(in + pos10, s_comp, gap_data, s_size);
                if (rs != s_size) { free(gap_data); free(covered); return 0; }
            } else {
                if (s_off + s_size > out_size) { free(covered); free(gap_data); return 0; }
                size_t rs = krv_decompress_block(in + pos10, s_comp, out + s_off, s_size);
                if (rs != s_size) { free(covered); free(gap_data); return 0; }
                memset(covered + s_off, 1, s_size);
            }
            pos10 += s_comp;
        }
        /* Place gap bytes */
        if (gap_data) {
            uint32_t gp = 0;
            for (size_t gi = 0; gi < out_size; gi++)
                if (!covered[gi]) out[gi] = gap_data[gp++];
            free(gap_data);
        }
        free(covered);
        return out_size;
    } else if (method == 11) {
        /* Hybrid ELF split: code(BCJ) + stride sections + combined rest */
        if (in_size < 18) return 0;
        uint8_t n_stride = in[1];
        uint32_t code_off, code_size, comp_code_size, comp_rest_size;
        memcpy(&code_off, in + 2, 4);
        memcpy(&code_size, in + 6, 4);
        memcpy(&comp_code_size, in + 10, 4);
        memcpy(&comp_rest_size, in + 14, 4);
        size_t hdr11 = 18 + (size_t)n_stride * 12;
        if (in_size < hdr11 + comp_code_size) return 0;

        /* Decompress code */
        uint8_t *code_buf = malloc(code_size);
        if (!code_buf) return 0;
        size_t rc = decompress_m1(in + hdr11, comp_code_size, code_buf, code_size);
        if (rc != code_size) { free(code_buf); return 0; }
        bcj_unfilter(code_buf, code_size, code_off);
        memcpy(out + code_off, code_buf, code_size);
        free(code_buf);

        /* Decompress stride sections */
        size_t pos11 = hdr11 + comp_code_size;
        typedef struct { uint32_t off, size, comp; } s11_t;
        s11_t secs11[16];
        for (int si = 0; si < n_stride; si++) {
            size_t base11 = 18 + (size_t)si * 12;
            memcpy(&secs11[si].off, in + base11, 4);
            memcpy(&secs11[si].size, in + base11 + 4, 4);
            memcpy(&secs11[si].comp, in + base11 + 8, 4);
            if (pos11 + secs11[si].comp > in_size) return 0;
            size_t rs = krv_decompress_block(in + pos11, secs11[si].comp, out + secs11[si].off, secs11[si].size);
            if (rs != secs11[si].size) return 0;
            pos11 += secs11[si].comp;
        }

        /* Decompress combined rest */
        uint32_t rest_size = (uint32_t)out_size - code_size;
        for (int si = 0; si < n_stride; si++) rest_size -= secs11[si].size;
        if (rest_size > 0 && comp_rest_size > 0) {
            if (pos11 + comp_rest_size > in_size) return 0;
            uint8_t *rest_buf = malloc(rest_size);
            if (!rest_buf) return 0;
            size_t rr = krv_decompress_block(in + pos11, comp_rest_size, rest_buf, rest_size);
            if (rr != rest_size) { free(rest_buf); return 0; }
            /* Scatter rest bytes back to their original positions */
            uint32_t rp = 0;
            uint32_t code_end = code_off + code_size;
            for (size_t gi = 0; gi < out_size; gi++) {
                if (gi >= code_off && gi < code_end) continue;
                int skip = 0;
                for (int si = 0; si < n_stride; si++)
                    if (gi >= secs11[si].off && gi < secs11[si].off + secs11[si].size)
                        { skip = 1; break; }
                if (skip) continue;
                out[gi] = rest_buf[rp++];
            }
            free(rest_buf);
        }
        return out_size;
    } else if (method >= 4 && method <= 5) {
        /* Stride-transposed methods */
        if (in_size < 3) return 0;
        int stride = in[1];
        if (stride == 0) return 0;
        /* Decompress into temp buffer */
        uint8_t *tmp = malloc(out_size);
        if (!tmp) return 0;
        r = decompress_m1(in + 2, in_size - 2, tmp, out_size);
        if (r > 0) {
            if (method == 5) bcj_unfilter(tmp, out_size, 0);
            untranspose(tmp, out, out_size, stride);
        }
        free(tmp);
    } else if (method == 12) {
        /* .eh_frame stream separation */
        if (in_size < 33) return 0;
        uint32_t nrec, n_fde, flags_sz, cl, cpb, cpr, cr, cb;
        memcpy(&nrec, in+1, 4); memcpy(&n_fde, in+5, 4);
        memcpy(&flags_sz, in+9, 4); memcpy(&cl, in+13, 4);
        memcpy(&cpb, in+17, 4); memcpy(&cpr, in+21, 4);
        memcpy(&cr, in+25, 4); memcpy(&cb, in+29, 4);
        size_t pos12 = 33;
        if (pos12 + flags_sz > in_size) return 0;
        const uint8_t *flags = in + pos12; pos12 += flags_sz;
        uint8_t *lengths = malloc((size_t)nrec*2);
        uint8_t *pc_begins = malloc((size_t)n_fde*4);
        uint8_t *pc_ranges = malloc((size_t)n_fde*4);
        uint8_t *cie_refs = malloc(n_fde);
        uint8_t *bodies = malloc(out_size);
        if (!lengths||!pc_begins||!pc_ranges||!cie_refs||!bodies) {
            free(lengths);free(pc_begins);free(pc_ranges);free(cie_refs);free(bodies); return 0;
        }
        if (pos12+cl>in_size) goto m12_fail;
        size_t rl=decompress_m1(in+pos12,cl,lengths,(size_t)nrec*2); pos12+=cl;
        if (pos12+cpb>in_size) goto m12_fail;
        size_t rpb=decompress_m1(in+pos12,cpb,pc_begins,(size_t)n_fde*4); pos12+=cpb;
        if (pos12+cpr>in_size) goto m12_fail;
        size_t rpr=decompress_m1(in+pos12,cpr,pc_ranges,(size_t)n_fde*4); pos12+=cpr;
        if (pos12+cr>in_size) goto m12_fail;
        size_t rcr=decompress_m1(in+pos12,cr,cie_refs,n_fde); pos12+=cr;
        if (pos12+cb>in_size) goto m12_fail;
        size_t rb=decompress_m1(in+pos12,cb,bodies,out_size);
        if (rl!=(size_t)nrec*2||rpb!=(size_t)n_fde*4||rpr!=(size_t)n_fde*4||rcr!=n_fde||rb==0)
            goto m12_fail;
        {
            size_t out_pos=0, body_pos=0;
            int32_t prev_pc=0;
            int fde_idx=0, n_cie=0;
            uint32_t cie_positions[16];
            for (uint32_t ri=0; ri<nrec; ri++) {
                uint16_t l16; memcpy(&l16, lengths+ri*2, 2);
                uint32_t rlen=l16;
                if (out_pos+4+rlen>out_size) break;
                memcpy(out+out_pos, &rlen, 4);
                int is_fde = (flags[ri/8]>>(ri&7))&1;
                if (!is_fde) {
                    if (n_cie<16) cie_positions[n_cie++]=(uint32_t)out_pos;
                    memcpy(out+out_pos+4, bodies+body_pos, rlen);
                    body_pos += rlen;
                } else {
                    uint8_t cidx = cie_refs[fde_idx];
                    uint32_t cie_abs = (cidx<n_cie) ? cie_positions[cidx] : 0;
                    uint32_t cie_ptr = (uint32_t)(out_pos+4) - cie_abs;
                    memcpy(out+out_pos+4, &cie_ptr, 4);
                    int32_t delta; memcpy(&delta, pc_begins+fde_idx*4, 4);
                    int32_t pc_begin = prev_pc+delta; prev_pc=pc_begin;
                    memcpy(out+out_pos+8, &pc_begin, 4);
                    uint32_t pc_range; memcpy(&pc_range, pc_ranges+fde_idx*4, 4);
                    memcpy(out+out_pos+12, &pc_range, 4);
                    fde_idx++;
                    if (rlen>12 && body_pos+rlen-12<=rb) {
                        memcpy(out+out_pos+16, bodies+body_pos, rlen-12);
                        body_pos += rlen-12;
                    }
                }
                out_pos += 4+rlen;
            }
            r = out_pos;
            /* Emit null terminator if space allows */
            if (out_pos + 4 <= out_size) {
                memset(out + out_pos, 0, 4);
                r = out_pos + 4;
            }
        }
        if (0) { m12_fail: r=0; }
        free(lengths);free(pc_begins);free(pc_ranges);free(cie_refs);free(bodies);
    } else if (method == 13) {
        /* Content-type separation: string vs binary blocks */
        if (in_size < 9) return 0;
        uint32_t nblk, str_comp;
        memcpy(&nblk, in+1, 4); memcpy(&str_comp, in+5, 4);
        uint32_t bitmap_sz = (nblk + 7) / 8;
        size_t hdr13 = 9 + bitmap_sz;
        if (in_size < hdr13) return 0;
        const uint8_t *bitmap = in + 9;
        size_t pos13 = hdr13;
        uint32_t str_size = 0, bin_size = 0;
        for (uint32_t bi = 0; bi < nblk; bi++) {
            if ((bitmap[bi/8] >> (bi&7)) & 1) str_size += 64;
            else bin_size += 64;
        }
        uint32_t tail = (uint32_t)(out_size - (size_t)nblk * 64);
        bin_size += tail;
        uint8_t *str_buf = malloc(str_size);
        uint8_t *bin_buf = malloc(bin_size);
        if (!str_buf || !bin_buf) { free(str_buf); free(bin_buf); return 0; }
        if (pos13 + str_comp > in_size) { free(str_buf); free(bin_buf); return 0; }
        size_t rs = krv_decompress_block(in + pos13, str_comp, str_buf, str_size);
        if (rs != str_size) { free(str_buf); free(bin_buf); return 0; }
        pos13 += str_comp;
        size_t rb = krv_decompress_block(in + pos13, in_size - pos13, bin_buf, bin_size);
        if (rb != bin_size) { free(str_buf); free(bin_buf); return 0; }
        /* Interleave back */
        uint32_t sp = 0, bp = 0;
        for (uint32_t bi = 0; bi < nblk; bi++) {
            if ((bitmap[bi/8] >> (bi&7)) & 1)
                { memcpy(out + (size_t)bi*64, str_buf + sp, 64); sp += 64; }
            else
                { memcpy(out + (size_t)bi*64, bin_buf + bp, 64); bp += 64; }
        }
        if (tail) memcpy(out + (size_t)nblk*64, bin_buf + bp, tail);
        free(str_buf); free(bin_buf);
        return out_size;
    } else if (method == 6 || method == 7) {
        /* ELF-aware BCJ methods */
        switch (method & 1) {
            case 0: r = decompress_m0(in+1, in_size-1, out, out_size); break;
            case 1: r = decompress_m1(in+1, in_size-1, out, out_size); break;
        }
        if (r > 0) bcj_unfilter_elf(out, out_size);
    } else {
        switch (method & 1) {
            case 0: r = decompress_m0(in+1, in_size-1, out, out_size); break;
            case 1: r = decompress_m1(in+1, in_size-1, out, out_size); break;
        }
        /* Undo BCJ if methods 2 or 3 */
        if (r > 0 && method >= 2) {
            bcj_unfilter(out, out_size, 0);
        }
    }
    return r;
}
