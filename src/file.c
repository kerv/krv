#include "krv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* CRC-32 (standard polynomial) */
static uint32_t crc32_table[256];
static int crc32_initialized = 0;

static void crc32_init(void) {
    if (crc32_initialized) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c >> 1) ^ (0xEDB88320 & (-(c & 1)));
        crc32_table[i] = c;
    }
    crc32_initialized = 1;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *buf, size_t len) {
    crc = ~crc;
    for (size_t i = 0; i < len; i++)
        crc = crc32_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void write_u32(FILE *f, uint32_t v) {
    uint8_t b[4] = {v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF};
    fwrite(b, 1, 4, f);
}

static void write_u64(FILE *f, uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (v >> (i * 8)) & 0xFF;
    fwrite(b, 1, 8, f);
}

static uint32_t read_u32(FILE *f) {
    uint8_t b[4];
    fread(b, 1, 4, f);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint64_t read_u64(FILE *f) {
    uint8_t b[8];
    fread(b, 1, 8, f);
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= ((uint64_t)b[i]) << (i * 8);
    return v;
}

int krv_compress_file(const char *in_path, const char *out_path, uint32_t block_size) {
    crc32_init();
    if (block_size == 0) block_size = KRV_DEFAULT_BLOCK_SIZE;

    FILE *fin = fopen(in_path, "rb");
    if (!fin) return -1;

    /* Get file size */
    fseek(fin, 0, SEEK_END);
    uint64_t file_size = (uint64_t)ftell(fin);
    fseek(fin, 0, SEEK_SET);

    uint32_t block_count = (uint32_t)((file_size + block_size - 1) / block_size);

    FILE *fout = fopen(out_path, "wb");
    if (!fout) { fclose(fin); return -1; }

    /* Write header */
    fwrite(KRV_MAGIC, 1, 4, fout);
    fputc(KRV_VERSION, fout);
    fputc(KRV_FLAG_CHECKSUM, fout);  /* flags: checksum enabled */
    write_u64(fout, file_size);
    write_u32(fout, block_size);
    write_u32(fout, block_count);

    /* Allocate buffers */
    uint8_t *in_buf = malloc(block_size);
    /* Worst case: incompressible data + overhead */
    size_t out_buf_size = block_size + block_size / 8 + 256;
    uint8_t *out_buf = malloc(out_buf_size);
    if (!in_buf || !out_buf) {
        free(in_buf); free(out_buf);
        fclose(fin); fclose(fout);
        return -1;
    }

    uint32_t crc = 0;

    for (uint32_t b = 0; b < block_count; b++) {
        size_t to_read = block_size;
        if (b == block_count - 1 && file_size % block_size != 0)
            to_read = (size_t)(file_size % block_size);

        size_t nread = fread(in_buf, 1, to_read, fin);
        if (nread != to_read) {
            free(in_buf); free(out_buf);
            fclose(fin); fclose(fout);
            return -1;
        }

        crc = crc32_update(crc, in_buf, nread);

        size_t compressed = krv_compress_block(in_buf, nread, out_buf, out_buf_size);
        if (compressed == 0 || compressed >= nread) {
            /* Store uncompressed (flag: high bit of size set) */
            write_u32(fout, (uint32_t)nread | 0x80000000u);
            fwrite(in_buf, 1, nread, fout);
        } else {
            write_u32(fout, (uint32_t)compressed);
            fwrite(out_buf, 1, compressed, fout);
        }
    }

    /* Write CRC */
    write_u32(fout, crc);

    free(in_buf); free(out_buf);
    fclose(fin); fclose(fout);
    return 0;
}

int krv_decompress_file(const char *in_path, const char *out_path) {
    crc32_init();

    FILE *fin = fopen(in_path, "rb");
    if (!fin) return -1;

    /* Read and verify header */
    char magic[4];
    fread(magic, 1, 4, fin);
    if (memcmp(magic, KRV_MAGIC, 4) != 0) { fclose(fin); return -2; }

    int version = fgetc(fin);
    if (version != KRV_VERSION) { fclose(fin); return -3; }

    int flags = fgetc(fin);
    uint64_t file_size = read_u64(fin);
    uint32_t block_size = read_u32(fin);
    uint32_t block_count = read_u32(fin);

    FILE *fout = fopen(out_path, "wb");
    if (!fout) { fclose(fin); return -1; }

    /* Allocate buffers */
    size_t comp_buf_size = block_size + block_size / 8 + 256;
    uint8_t *comp_buf = malloc(comp_buf_size);
    uint8_t *out_buf = malloc(block_size);
    if (!comp_buf || !out_buf) {
        free(comp_buf); free(out_buf);
        fclose(fin); fclose(fout);
        return -1;
    }

    uint32_t crc = 0;
    uint64_t remaining = file_size;

    for (uint32_t b = 0; b < block_count; b++) {
        uint32_t block_header = read_u32(fin);
        size_t orig_size = (remaining < block_size) ? (size_t)remaining : block_size;

        if (block_header & 0x80000000u) {
            /* Stored uncompressed */
            size_t stored_size = block_header & 0x7FFFFFFFu;
            fread(out_buf, 1, stored_size, fin);
            crc = crc32_update(crc, out_buf, stored_size);
            fwrite(out_buf, 1, stored_size, fout);
            remaining -= stored_size;
        } else {
            /* Compressed block */
            size_t comp_size = block_header;
            fread(comp_buf, 1, comp_size, fin);
            size_t decompressed = krv_decompress_block(comp_buf, comp_size, out_buf, orig_size);
            if (decompressed != orig_size) {
                free(comp_buf); free(out_buf);
                fclose(fin); fclose(fout);
                return -4;
            }
            crc = crc32_update(crc, out_buf, decompressed);
            fwrite(out_buf, 1, decompressed, fout);
            remaining -= decompressed;
        }
    }

    /* Verify CRC if present */
    if (flags & KRV_FLAG_CHECKSUM) {
        uint32_t stored_crc = read_u32(fin);
        if (stored_crc != crc) {
            free(comp_buf); free(out_buf);
            fclose(fin); fclose(fout);
            return -5;
        }
    }

    free(comp_buf); free(out_buf);
    fclose(fin); fclose(fout);
    return 0;
}
