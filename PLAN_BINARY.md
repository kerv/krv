# Plan: Beat 7z on Binary Files

## Current State (Phase 7 + 4M blocks + 16K OPT_WINDOW)

| File | KRV | 7z | Gap |
|------|-----|-----|-----|
| services | **5,064** | 5,241 | **-3.4%** ✓ |
| copyright | 23,952 | 23,351 | +2.6% |
| ls | 55,220 | 52,383 | +5.4% |
| bash | 601,702 | 545,073 | +10.4% |
| git | 1,745,670 | 1,550,976 | +12.5% |
| libc | 835,069 | 743,402 | +12.3% |
| logs | 28,076 | 25,074 | +12.0% |
| json | **4,085** | 6,563 | **-37.7%** ✓ |
| source | **1,199** | 1,355 | **-11.5%** ✓ |

Beats 7z: 3/9 (services, json, source). Beats zstd: 9/9.

## Root Cause of Binary Gap

- `.text` (code, 67% of ELF) **already beats 7z** (-0.5%) — BCJ + CM works great
- Gap comes from **structured metadata sections**:
  - `.eh_frame`: +23.9% vs 7z (91KB in bash)
  - `.dynsym`: +12.8% vs 7z (62KB in bash)
  - `.rodata`: +5.5% vs 7z (106KB in bash)
- Column-split (transpose) on `.dynsym` at stride 24 → beats 7z by 6.4%
- LZMA's matched-literal encoding gives ~0.5 bit/byte advantage on binary literals

## Approach 1: Auto-Stride Detection + Column-Split (HIGHEST IMPACT)

**Idea:** Detect fixed-record-size regions and transpose them before compression.

**How it works:**
1. During pass 1, measure byte autocorrelation at strides 2,4,8,12,16,20,24,32
2. If stride S shows >60% autocorrelation over a 4KB+ region, apply column-split
3. Column-split = transpose: group all byte[0]s, byte[1]s, ..., byte[S-1]s
4. Store stride in block header so decoder can reverse it

**Why it's novel:** No mainstream compressor does automatic stride detection. Some (like bsc) do BWT which partially captures this, but column-split is more targeted and cheaper.

**Proven results:**
- `.dynsym` stride-24 transpose: 14,745 (normal: 17,725, 7z: 15,754) — beats 7z by 6.4%
- Whole-file transpose: terrible (-45%) — must be selective

**Challenges:**
- Must detect stride boundaries accurately (don't transpose .text!)
- Need to handle variable-length records (eh_frame) differently
- Adds complexity to format (stride stored per-block or per-region)

## Approach 2: Matched-Literal CM Predictor (MEDIUM-HIGH IMPACT)

**Idea:** After a match, use the "match byte" (byte at same offset in match source) as an additional CM predictor.

**How it works:**
1. Track the last match distance in the CM state
2. When encoding a literal after a match, look up `data[pos - last_match_dist]`
3. Add a 9th predictor that uses XOR(actual_bit, match_byte_bit) as context
4. The mixer learns to weight this predictor heavily when it's accurate

**Why it's novel:** LZMA uses a dedicated bit-level XOR tree for matched literals. Our approach integrates it into the CM framework, letting the mixer adaptively decide when to trust it.

**Expected impact:** ~0.3-0.5 bits/byte on binary literals after matches. On bash's .text section (968KB), that's potentially 36-60KB savings.

**Challenges:**
- Need to track "last match distance" through the CM
- Only useful for literals immediately after matches
- May interfere with existing predictor weights

## Approach 3: Larger LZ Window (LOWER PRIORITY)

Increase from 4MB to 8MB or 16MB. Simple but only helps files > 4MB.

## Implementation Order

1. **Auto-stride** — DONE. Works but doesn't trigger on ELF (structured sections too small a fraction). Will help on pure structured data files.
2. **Matched-literal** — DONE. 9th CM predictor with 3/4 strength. Helps binaries (libc -2,314, git -1,261, bash -636). Tiny text regressions (services +18, json +66) — mixer learns to ignore it.
3. **Larger OPT window (16K)** — DONE. Helps most files significantly.
4. **4M block size** — DONE. Eliminates cross-block losses on large files.

## Current Results (Phase 8)

| File | KRV | 7z | Gap |
|------|-----|-----|-----|
| services | **5,082** | 5,241 | **-3.0%** ✓ |
| copyright | 24,004 | 23,351 | +2.8% |
| ls | 55,127 | 52,383 | +5.2% |
| bash | 601,066 | 545,073 | +10.3% |
| git | 1,744,409 | 1,550,976 | +12.5% |
| libc | 832,755 | 743,402 | +12.0% |
| logs | 28,112 | 25,074 | +12.1% |
| json | **4,151** | 6,563 | **-36.7%** ✓ |
| source | **1,189** | 1,355 | **-12.3%** ✓ |

Beats 7z: 3/9. Beats zstd: 9/9.

## What We Tried and Didn't Work

- Position-dependent dist slots (4 trees, each gets 1/4 training data → worse)
- Distance-dependent min match length (DP already handles this via pricing)
- 2-byte hash chain (helps binaries 0.4-0.8% but kills JSON)
- Length-dependent pricing bonus (neutral)
- Delta filter (destroys CM patterns)
- Whole-file transpose (destroys LZ matches)
