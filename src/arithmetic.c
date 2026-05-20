#include "krv.h"

void ac_enc_init(ac_encoder_t *e, uint8_t *buf, size_t size) {
    e->lo = 0;
    e->hi = 0xFFFFFFFF;
    e->buf = buf;
    e->buf_size = size;
    e->buf_pos = 0;
}

static inline void ac_enc_byte(ac_encoder_t *e, uint8_t b) {
    if (e->buf_pos < e->buf_size)
        e->buf[e->buf_pos++] = b;
}

void ac_enc_bit(ac_encoder_t *e, int bit, uint32_t prob) {
    /* prob is P(bit=1) in PROB_BITS fixed point (0..4096) */
    uint32_t mid = e->lo + (uint32_t)(((uint64_t)(e->hi - e->lo) * prob) >> PROB_BITS);
    if (bit)
        e->hi = mid;
    else
        e->lo = mid + 1;

    /* Renormalize */
    while ((e->lo ^ e->hi) < AC_TOP) {
        ac_enc_byte(e, (uint8_t)(e->lo >> 24));
        e->lo <<= 8;
        e->hi = (e->hi << 8) | 0xFF;
    }
}

size_t ac_enc_flush(ac_encoder_t *e) {
    /* Flush remaining bytes */
    ac_enc_byte(e, (uint8_t)(e->lo >> 24));
    ac_enc_byte(e, (uint8_t)(e->lo >> 16));
    ac_enc_byte(e, (uint8_t)(e->lo >> 8));
    ac_enc_byte(e, (uint8_t)(e->lo));
    return e->buf_pos;
}

void ac_dec_init(ac_decoder_t *d, const uint8_t *buf, size_t size) {
    d->lo = 0;
    d->hi = 0xFFFFFFFF;
    d->buf = buf;
    d->buf_size = size;
    d->buf_pos = 0;
    d->code = 0;
    /* Read initial 4 bytes */
    for (int i = 0; i < 4; i++) {
        d->code = (d->code << 8);
        if (d->buf_pos < d->buf_size)
            d->code |= d->buf[d->buf_pos++];
    }
}

int ac_dec_bit(ac_decoder_t *d, uint32_t prob) {
    uint32_t mid = d->lo + (uint32_t)(((uint64_t)(d->hi - d->lo) * prob) >> PROB_BITS);
    int bit = (d->code <= mid);

    if (bit)
        d->hi = mid;
    else
        d->lo = mid + 1;

    /* Renormalize */
    while ((d->lo ^ d->hi) < AC_TOP) {
        d->lo <<= 8;
        d->hi = (d->hi << 8) | 0xFF;
        d->code <<= 8;
        if (d->buf_pos < d->buf_size)
            d->code |= d->buf[d->buf_pos++];
    }
    return bit;
}
