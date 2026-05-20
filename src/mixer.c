#include "krv.h"

/*
 * Logistic mixer: combines predictions using learned weights.
 * P_combined = sigmoid(sum(w[i] * stretch(p[i])))
 * where stretch(p) = log(p / (1-p)) (logit)
 *
 * We use fixed-point lookup tables for stretch/squash.
 */

/* Stretch table: maps probability (0..4096) -> logit in fixed point (scaled by 256) */
/* Precomputed: stretch[p] = round(256 * ln(p / (4096-p))) */
int16_t stretch_table[PROB_ONE + 1];
/* Squash table: maps logit -> probability */
uint16_t squash_table[4097]; /* index = logit + 2048, range [-2048..2048] */

static int tables_initialized = 0;

static void init_tables(void) {
    if (tables_initialized) return;
    /* Compute stretch: logit function scaled */
    for (int i = 1; i < PROB_ONE; i++) {
        double p = (double)i / PROB_ONE;
        double logit = 0.0;
        /* Use log(p/(1-p)) */
        if (p < 0.0001) p = 0.0001;
        if (p > 0.9999) p = 0.9999;
        logit = 2.0 * 256.0 * (p - 0.5); /* linear approximation for speed */
        /* Better: actual logit */
        double pp = (double)i / PROB_ONE;
        if (pp < 0.001) pp = 0.001;
        if (pp > 0.999) pp = 0.999;
        logit = 256.0 * __builtin_log(pp / (1.0 - pp));
        if (logit > 32000) logit = 32000;
        if (logit < -32000) logit = -32000;
        stretch_table[i] = (int16_t)logit;
    }
    stretch_table[0] = -32000;
    stretch_table[PROB_ONE] = 32000;

    /* Compute squash: sigmoid function */
    for (int i = 0; i < 4097; i++) {
        double x = (double)(i - 2048) / 256.0;
        double p = 1.0 / (1.0 + __builtin_exp(-x));
        int v = (int)(p * PROB_ONE + 0.5);
        if (v < 1) v = 1;
        if (v > PROB_ONE - 1) v = PROB_ONE - 1;
        squash_table[i] = (uint16_t)v;
    }
    tables_initialized = 1;
}

static inline int16_t stretch(uint32_t p) {
    if (p >= PROB_ONE) p = PROB_ONE - 1;
    if (p == 0) p = 1;
    return stretch_table[p];
}

static inline uint32_t squash(int32_t x) {
    /* x is logit * 256, map to [0..4096] */
    int idx = (int)(x / 1) + 2048; /* x is already in right scale */
    if (idx < 0) idx = 0;
    if (idx > 4096) idx = 4096;
    return squash_table[idx];
}

void mixer_init(mixer_t *m) {
    init_tables();
    m->ctx = 0;
    for (int c = 0; c < MIXER_CONTEXTS; c++)
        for (int i = 0; i < NUM_PREDICTORS; i++)
            m->weights[c][i] = 256;
}

uint32_t mixer_predict(mixer_t *m, uint32_t *preds) {
    int32_t sum = 0;
    int c = m->ctx;
    int32_t *w = m->weights[c];
    for (int i = 0; i < NUM_PREDICTORS; i++) {
        int16_t s = stretch_table[preds[i]];
        m->last_stretch[i] = s;
        sum += (w[i] * (int32_t)s) >> 8;
    }
    if (sum < -2048) sum = -2048;
    if (sum > 2048) sum = 2048;
    m->last_sum = sum;
    return squash_table[sum + 2048];
}

void mixer_update(mixer_t *m, uint32_t *preds, int bit) {
    (void)preds;
    int c = m->ctx;
    uint32_t p = squash_table[m->last_sum + 2048];
    int32_t error = (int32_t)(bit ? PROB_ONE : 0) - (int32_t)p;
    int32_t *w = m->weights[c];

    for (int i = 0; i < NUM_PREDICTORS; i++) {
        int32_t delta = (error * (int32_t)m->last_stretch[i]) >> 16;
        w[i] += delta;
        if (w[i] > 2048) w[i] = 2048;
        if (w[i] < -2048) w[i] = -2048;
    }
}
