#include "krv.h"
#include <string.h>

/*
 * BCJ (Branch Conversion for Jump) filter for x86 code.
 * Converts relative CALL (E8) and JMP (E9) addresses to absolute.
 * This makes repeated calls to the same function produce identical byte sequences.
 */

void bcj_filter(uint8_t *data, size_t size, size_t start_ip) {
    if (size < 5) return;
    for (size_t i = 0; i + 4 < size; i++) {
        if (data[i] == 0xE8 || data[i] == 0xE9) {
            uint32_t rel = (uint32_t)data[i+1] |
                           ((uint32_t)data[i+2] << 8) |
                           ((uint32_t)data[i+3] << 16) |
                           ((uint32_t)data[i+4] << 24);
            uint32_t abs_addr = rel + (uint32_t)(start_ip + i + 5);
            data[i+1] = (uint8_t)(abs_addr);
            data[i+2] = (uint8_t)(abs_addr >> 8);
            data[i+3] = (uint8_t)(abs_addr >> 16);
            data[i+4] = (uint8_t)(abs_addr >> 24);
            i += 4;
        } else if (i + 5 < size &&
                   (data[i] == 0x8D || data[i] == 0x8B) &&
                   (data[i+1] & 0xC7) == 0x05) {
            /* Non-REX LEA/MOV with RIP-relative (mod=00, rm=101), 6 bytes total */
            uint32_t rel = (uint32_t)data[i+2] |
                           ((uint32_t)data[i+3] << 8) |
                           ((uint32_t)data[i+4] << 16) |
                           ((uint32_t)data[i+5] << 24);
            uint32_t abs_addr = rel + (uint32_t)(start_ip + i + 6);
            data[i+2] = (uint8_t)(abs_addr);
            data[i+3] = (uint8_t)(abs_addr >> 8);
            data[i+4] = (uint8_t)(abs_addr >> 16);
            data[i+5] = (uint8_t)(abs_addr >> 24);
            i += 5;
        } else if (i + 6 < size && data[i] >= 0x40 && data[i] <= 0x4F &&
                   (data[i+1] == 0x8D || data[i+1] == 0x8B || data[i+1] == 0x89 ||
                    data[i+1] == 0x3B || data[i+1] == 0x39) &&
                   (data[i+2] & 0xC7) == 0x05) {
            /* REX + LEA/MOV/CMP with RIP-relative addressing (mod=00, rm=101) */
            uint32_t rel = (uint32_t)data[i+3] |
                           ((uint32_t)data[i+4] << 8) |
                           ((uint32_t)data[i+5] << 16) |
                           ((uint32_t)data[i+6] << 24);
            uint32_t abs_addr = rel + (uint32_t)(start_ip + i + 7);
            data[i+3] = (uint8_t)(abs_addr);
            data[i+4] = (uint8_t)(abs_addr >> 8);
            data[i+5] = (uint8_t)(abs_addr >> 16);
            data[i+6] = (uint8_t)(abs_addr >> 24);
            i += 6;
        } else if (i + 9 < size && data[i] >= 0x40 && data[i] <= 0x4F &&
                   data[i+1] == 0xC7 && (data[i+2] & 0xC7) == 0x05) {
            /* REX + MOV [rip+disp32], imm32 — 10-byte instruction */
            uint32_t rel = (uint32_t)data[i+3] |
                           ((uint32_t)data[i+4] << 8) |
                           ((uint32_t)data[i+5] << 16) |
                           ((uint32_t)data[i+6] << 24);
            uint32_t abs_addr = rel + (uint32_t)(start_ip + i + 10);
            data[i+3] = (uint8_t)(abs_addr);
            data[i+4] = (uint8_t)(abs_addr >> 8);
            data[i+5] = (uint8_t)(abs_addr >> 16);
            data[i+6] = (uint8_t)(abs_addr >> 24);
            i += 9;
        }
    }
}

void bcj_unfilter(uint8_t *data, size_t size, size_t start_ip) {
    if (size < 5) return;
    for (size_t i = 0; i + 4 < size; i++) {
        if (data[i] == 0xE8 || data[i] == 0xE9) {
            uint32_t abs_addr = (uint32_t)data[i+1] |
                                ((uint32_t)data[i+2] << 8) |
                                ((uint32_t)data[i+3] << 16) |
                                ((uint32_t)data[i+4] << 24);
            uint32_t rel = abs_addr - (uint32_t)(start_ip + i + 5);
            data[i+1] = (uint8_t)(rel);
            data[i+2] = (uint8_t)(rel >> 8);
            data[i+3] = (uint8_t)(rel >> 16);
            data[i+4] = (uint8_t)(rel >> 24);
            i += 4;
        } else if (i + 5 < size &&
                   (data[i] == 0x8D || data[i] == 0x8B) &&
                   (data[i+1] & 0xC7) == 0x05) {
            /* Non-REX LEA/MOV with RIP-relative */
            uint32_t abs_addr = (uint32_t)data[i+2] |
                                ((uint32_t)data[i+3] << 8) |
                                ((uint32_t)data[i+4] << 16) |
                                ((uint32_t)data[i+5] << 24);
            uint32_t rel = abs_addr - (uint32_t)(start_ip + i + 6);
            data[i+2] = (uint8_t)(rel);
            data[i+3] = (uint8_t)(rel >> 8);
            data[i+4] = (uint8_t)(rel >> 16);
            data[i+5] = (uint8_t)(rel >> 24);
            i += 5;
        } else if (i + 6 < size && data[i] >= 0x40 && data[i] <= 0x4F &&
                   (data[i+1] == 0x8D || data[i+1] == 0x8B || data[i+1] == 0x89 ||
                    data[i+1] == 0x3B || data[i+1] == 0x39) &&
                   (data[i+2] & 0xC7) == 0x05) {
            /* REX + LEA/MOV/CMP with RIP-relative addressing */
            uint32_t abs_addr = (uint32_t)data[i+3] |
                                ((uint32_t)data[i+4] << 8) |
                                ((uint32_t)data[i+5] << 16) |
                                ((uint32_t)data[i+6] << 24);
            uint32_t rel = abs_addr - (uint32_t)(start_ip + i + 7);
            data[i+3] = (uint8_t)(rel);
            data[i+4] = (uint8_t)(rel >> 8);
            data[i+5] = (uint8_t)(rel >> 16);
            data[i+6] = (uint8_t)(rel >> 24);
            i += 6;
        } else if (i + 9 < size && data[i] >= 0x40 && data[i] <= 0x4F &&
                   data[i+1] == 0xC7 && (data[i+2] & 0xC7) == 0x05) {
            /* REX + MOV [rip+disp32], imm32 — 10-byte instruction */
            uint32_t abs_addr = (uint32_t)data[i+3] |
                                ((uint32_t)data[i+4] << 8) |
                                ((uint32_t)data[i+5] << 16) |
                                ((uint32_t)data[i+6] << 24);
            uint32_t rel = abs_addr - (uint32_t)(start_ip + i + 10);
            data[i+3] = (uint8_t)(rel);
            data[i+4] = (uint8_t)(rel >> 8);
            data[i+5] = (uint8_t)(rel >> 16);
            data[i+6] = (uint8_t)(rel >> 24);
            i += 9;
        }
    }
}

/* Detect if data looks like x86 code (heuristic) */
int bcj_detect_x86(const uint8_t *data, size_t size) {
    if (size < 64) return 0;

    /* Check ELF magic */
    if (data[0] == 0x7F && data[1] == 'E' && data[2] == 'L' && data[3] == 'F')
        return 1;

    /* Count E8/E9 bytes with plausible relative offsets */
    int e8_count = 0;
    int plausible = 0;
    size_t check = size < 4096 ? size : 4096;
    for (size_t i = 0; i + 4 < check; i++) {
        if (data[i] == 0xE8 || data[i] == 0xE9) {
            e8_count++;
            int32_t rel = (int32_t)((uint32_t)data[i+1] |
                          ((uint32_t)data[i+2] << 8) |
                          ((uint32_t)data[i+3] << 16) |
                          ((uint32_t)data[i+4] << 24));
            /* Plausible if target is within file bounds */
            int32_t target = rel + (int32_t)(i + 5);
            if (target >= 0 && (size_t)target < size) plausible++;
        }
    }
    /* If >1% of bytes are E8/E9 and >50% have plausible targets */
    return (e8_count > (int)(check / 100) && plausible * 2 > e8_count);
}

/* ELF-aware BCJ: only apply E8/E9 filter to executable sections */
void bcj_filter_elf(uint8_t *data, size_t size) {
    if (size < 64) { bcj_filter(data, size, 0); return; }
    /* Check ELF magic */
    if (!(data[0]==0x7F && data[1]=='E' && data[2]=='L' && data[3]=='F')) {
        bcj_filter(data, size, 0); return;
    }
    int is64 = (data[4] == 2);
    /* Parse ELF header to find section headers */
    uint64_t sh_off; uint16_t sh_ent, sh_num;
    if (is64) {
        if (size < 64) { bcj_filter(data, size, 0); return; }
        memcpy(&sh_off, data + 40, 8);
        memcpy(&sh_ent, data + 58, 2);
        memcpy(&sh_num, data + 60, 2);
    } else {
        if (size < 52) { bcj_filter(data, size, 0); return; }
        uint32_t sh_off32; memcpy(&sh_off32, data + 32, 4); sh_off = sh_off32;
        memcpy(&sh_ent, data + 46, 2);
        memcpy(&sh_num, data + 48, 2);
    }
    if (sh_off == 0 || sh_num == 0 || sh_off + (uint64_t)sh_ent * sh_num > size) {
        bcj_filter(data, size, 0); return;
    }
    /* Iterate sections, filter only executable ones (SHF_EXECINSTR = 0x4) */
    int filtered = 0;
    for (uint16_t i = 0; i < sh_num; i++) {
        uint8_t *sh = data + sh_off + (uint64_t)i * sh_ent;
        uint64_t s_offset, s_size, s_flags;
        if (is64) {
            memcpy(&s_flags, sh + 8, 8);
            memcpy(&s_offset, sh + 24, 8);
            memcpy(&s_size, sh + 32, 8);
        } else {
            uint32_t f32, o32, sz32;
            memcpy(&f32, sh + 8, 4);
            memcpy(&o32, sh + 16, 4);
            memcpy(&sz32, sh + 20, 4);
            s_flags = f32; s_offset = o32; s_size = sz32;
        }
        if ((s_flags & 0x4) && s_offset + s_size <= size && s_size >= 5) {
            bcj_filter(data + s_offset, s_size, s_offset);
            filtered = 1;
        }
    }
    if (!filtered) bcj_filter(data, size, 0);
}

void bcj_unfilter_elf(uint8_t *data, size_t size) {
    if (size < 64) { bcj_unfilter(data, size, 0); return; }
    if (!(data[0]==0x7F && data[1]=='E' && data[2]=='L' && data[3]=='F')) {
        bcj_unfilter(data, size, 0); return;
    }
    int is64 = (data[4] == 2);
    uint64_t sh_off; uint16_t sh_ent, sh_num;
    if (is64) {
        if (size < 64) { bcj_unfilter(data, size, 0); return; }
        memcpy(&sh_off, data + 40, 8);
        memcpy(&sh_ent, data + 58, 2);
        memcpy(&sh_num, data + 60, 2);
    } else {
        if (size < 52) { bcj_unfilter(data, size, 0); return; }
        uint32_t sh_off32; memcpy(&sh_off32, data + 32, 4); sh_off = sh_off32;
        memcpy(&sh_ent, data + 46, 2);
        memcpy(&sh_num, data + 48, 2);
    }
    if (sh_off == 0 || sh_num == 0 || sh_off + (uint64_t)sh_ent * sh_num > size) {
        bcj_unfilter(data, size, 0); return;
    }
    int filtered = 0;
    for (uint16_t i = 0; i < sh_num; i++) {
        uint8_t *sh = data + sh_off + (uint64_t)i * sh_ent;
        uint64_t s_offset, s_size, s_flags;
        if (is64) {
            memcpy(&s_flags, sh + 8, 8);
            memcpy(&s_offset, sh + 24, 8);
            memcpy(&s_size, sh + 32, 8);
        } else {
            uint32_t f32, o32, sz32;
            memcpy(&f32, sh + 8, 4);
            memcpy(&o32, sh + 16, 4);
            memcpy(&sz32, sh + 20, 4);
            s_flags = f32; s_offset = o32; s_size = sz32;
        }
        if ((s_flags & 0x4) && s_offset + s_size <= size && s_size >= 5) {
            bcj_unfilter(data + s_offset, s_size, s_offset);
            filtered = 1;
        }
    }
    if (!filtered) bcj_unfilter(data, size, 0);
}

/* Delta filter: XOR each byte with the byte `stride` positions back */
void delta_filter(uint8_t *data, size_t size, int stride) {
    if ((size_t)stride >= size) return;
    /* Process backwards to allow in-place */
    for (size_t i = size - 1; i >= (size_t)stride; i--)
        data[i] ^= data[i - stride];
}

void delta_unfilter(uint8_t *data, size_t size, int stride) {
    if ((size_t)stride >= size) return;
    for (size_t i = stride; i < size; i++)
        data[i] ^= data[i - stride];
}
