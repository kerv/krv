#ifndef KRV_H
#define KRV_H

#include <stdint.h>
#include <stddef.h>

/* File format constants */
#define KRV_MAGIC "KRV\x01"
#define KRV_VERSION 1
#define KRV_DEFAULT_BLOCK_SIZE (4 * 1024 * 1024)  /* 4MB */

/* Flag bits */
#define KRV_FLAG_CHECKSUM  (1 << 0)
#define KRV_FLAG_FILENAME  (1 << 1)

/* Arithmetic coder constants */
#define AC_PRECISION 32
#define AC_TOP       ((uint32_t)1 << 24)
#define AC_BOT       ((uint32_t)1 << 16)

/* Predictor constants */
#define PROB_BITS    12
#define PROB_ONE     (1 << PROB_BITS)  /* 4096 = 1.0 */
#define PROB_HALF    (PROB_ONE / 2)

/* Mixer constants */
#define NUM_PREDICTORS 9  /* o0, o1, o2, o4, o8, match, run-length, stride, lz-match-byte */
#define MIXER_LR_BITS  10
#define MIXER_LR       (1 << MIXER_LR_BITS)

/* Match finder constants */
#define MATCH_WINDOW   (64 * 1024)  /* 64KB sliding window */
#define MATCH_HASH_BITS 16
#define MATCH_HASH_SIZE (1 << MATCH_HASH_BITS)

/* --- Arithmetic Coder --- */
typedef struct {
    uint32_t lo, hi;
    uint8_t *buf;
    size_t   buf_size;
    size_t   buf_pos;
} ac_encoder_t;

typedef struct {
    uint32_t lo, hi, code;
    const uint8_t *buf;
    size_t   buf_size;
    size_t   buf_pos;
} ac_decoder_t;

void ac_enc_init(ac_encoder_t *e, uint8_t *buf, size_t size);
void ac_enc_bit(ac_encoder_t *e, int bit, uint32_t prob);
size_t ac_enc_flush(ac_encoder_t *e);

void ac_dec_init(ac_decoder_t *d, const uint8_t *buf, size_t size);
int  ac_dec_bit(ac_decoder_t *d, uint32_t prob);

/* --- Predictors --- */

/* Order-0: bit frequency per partial byte context */
typedef struct {
    uint16_t counts[256][2];
} predictor_o0_t;

/* Order-1: conditioned on previous byte */
typedef struct {
    uint16_t counts[256][256][2];
} predictor_o1_t;

/* Order-2: conditioned on previous 2 bytes (hashed) */
#define O2_HASH_SIZE (1 << 16)
typedef struct {
    uint16_t counts[O2_HASH_SIZE][2];
    uint16_t cached_idx;
} predictor_o2_t;

/* Order-4: conditioned on previous 4 bytes (hashed) */
#define O4_HASH_SIZE (1 << 19)
typedef struct {
    uint16_t counts[O4_HASH_SIZE][2];
    uint32_t cached_idx;
} predictor_o4_t;

/* Order-8: conditioned on previous 8 bytes (hashed) */
#define O8_HASH_SIZE (1 << 19)
typedef struct {
    uint16_t counts[O8_HASH_SIZE][2];
    uint32_t cached_idx;
} predictor_o8_t;

/* Match finder: predicts from longest match in sliding window */
typedef struct {
    uint8_t *window;       /* circular buffer */
    uint32_t *hash_table;  /* hash -> position in window */
    size_t   pos;          /* current position in stream */
    size_t   match_pos;    /* position of current match */
    size_t   match_len;    /* length of current match */
    uint8_t  match_byte;   /* predicted next byte from match */
    int      match_active; /* whether we have an active match */
} predictor_match_t;

/* Run-length: predicts continuation of byte runs */
typedef struct {
    uint8_t  run_byte;     /* current run byte */
    uint32_t run_length;   /* current run length */
    int      in_run;       /* whether we're in a run */
} predictor_rl_t;

/* Stride predictor: structural resonance detection */
#define STRIDE_BUF_SIZE    (64 * 1024)  /* must be power of 2 */
#define STRIDE_MAX         256
#define STRIDE_DETECT_MIN  64
#define STRIDE_DETECT_INTERVAL 2048

typedef struct {
    uint8_t  buf[STRIDE_BUF_SIZE];
    size_t   pos;
    uint32_t stride;       /* detected dominant stride (0 = none) */
    uint32_t confidence;   /* 0-256 autocorrelation strength */
    size_t   next_detect;  /* next position to re-run detection */
} predictor_stride_t;

void pred_o0_init(predictor_o0_t *p);
uint32_t pred_o0_predict(predictor_o0_t *p, uint8_t ctx);
void pred_o0_update(predictor_o0_t *p, uint8_t ctx, int bit);

void pred_o1_init(predictor_o1_t *p);
uint32_t pred_o1_predict(predictor_o1_t *p, uint8_t prev, uint8_t ctx);
void pred_o1_update(predictor_o1_t *p, uint8_t prev, uint8_t ctx, int bit);

void pred_o2_init(predictor_o2_t *p);
uint32_t pred_o2_predict(predictor_o2_t *p, uint8_t prev2, uint8_t prev1, uint8_t ctx);
void pred_o2_update(predictor_o2_t *p, uint8_t prev2, uint8_t prev1, uint8_t ctx, int bit);

void pred_o4_init(predictor_o4_t *p);
uint32_t pred_o4_predict(predictor_o4_t *p, const uint8_t *prev4, uint8_t ctx);
void pred_o4_update(predictor_o4_t *p, const uint8_t *prev4, uint8_t ctx, int bit);

void pred_o8_init(predictor_o8_t *p);
uint32_t pred_o8_predict(predictor_o8_t *p, const uint8_t *prev8, uint8_t ctx);
void pred_o8_update(predictor_o8_t *p, const uint8_t *prev8, uint8_t ctx, int bit);

void pred_match_init(predictor_match_t *p);
void pred_match_free(predictor_match_t *p);
void pred_match_update_byte(predictor_match_t *p, uint8_t byte);
uint32_t pred_match_predict(predictor_match_t *p, uint8_t ctx, int bit_idx);

void pred_rl_init(predictor_rl_t *p);
void pred_rl_update_byte(predictor_rl_t *p, uint8_t byte);
uint32_t pred_rl_predict(predictor_rl_t *p, uint8_t ctx, int bit_idx);

void pred_stride_init(predictor_stride_t *p);
void pred_stride_update_byte(predictor_stride_t *p, uint8_t byte);
uint32_t pred_stride_predict(predictor_stride_t *p, uint8_t ctx, int bit_idx);

/* --- Logistic Mixer --- */
#define MIXER_CONTEXTS 2  /* high nibble vs low nibble bit positions */
typedef struct {
    int32_t weights[MIXER_CONTEXTS][NUM_PREDICTORS];
    int32_t last_sum;  /* cached from predict for update */
    int16_t last_stretch[NUM_PREDICTORS]; /* cached stretched preds */
    int ctx;  /* current context (0-7) */
} mixer_t;

void mixer_init(mixer_t *m);
uint32_t mixer_predict(mixer_t *m, uint32_t *preds);
void mixer_update(mixer_t *m, uint32_t *preds, int bit);

/* --- Block compress/decompress --- */
size_t krv_compress_block(const uint8_t *in, size_t in_size,
                          uint8_t *out, size_t out_size);
size_t krv_decompress_block(const uint8_t *in, size_t in_size,
                            uint8_t *out, size_t out_size);

/* --- BCJ filter (x86 CALL/JMP transform) --- */
void bcj_filter(uint8_t *data, size_t size, size_t start_ip);
void bcj_unfilter(uint8_t *data, size_t size, size_t start_ip);
int bcj_detect_x86(const uint8_t *data, size_t size);
void bcj_filter_elf(uint8_t *data, size_t size);
void bcj_unfilter_elf(uint8_t *data, size_t size);
void delta_filter(uint8_t *data, size_t size, int stride);
void delta_unfilter(uint8_t *data, size_t size, int stride);

/* --- File-level API --- */
int krv_compress_file(const char *in_path, const char *out_path, uint32_t block_size);
int krv_decompress_file(const char *in_path, const char *out_path);

#endif /* KRV_H */
