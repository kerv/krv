# KRV Compression — Session State & Next Steps

## Current Results (Phase 20 — Speed Optimization III)

| File | Original | 7z | **KRV** | vs 7z |
|------|----------|-----|---------|-------|
| services (12KB text) | 12,813 | 5,241 | **4,892** | **-6.7%** ✓ |
| copyright (110KB text) | 110,653 | 23,351 | **23,263** | **-0.4%** ✓ |
| ls (142KB ELF) | 142,312 | 52,383 | **51,990** | **-0.8%** ✓ |
| bash (1.4MB ELF) | 1,446,024 | 545,073 | **515,059** | **-5.5%** ✓ |
| git (4MB ELF) | 4,066,232 | 1,550,976 | **1,495,410** | **-3.6%** ✓ |
| libc (2.1MB) | 2,125,328 | 743,402 | **749,745** | +0.9% |
| log data (~900KB) | 832,184 | 25,074 | **24,926** | **-0.6%** ✓ |
| json (~180KB) | 165,670 | 6,563 | **4,211** | **-35.8%** ✓ |
| source (~25KB) | 22,725 | 1,355 | **1,033** | **-23.8%** ✓ |

**Beats 7z: 8/9** (all except libc) | **Beats zstd: 9/9** | **Beats gzip: 9/9** | **Beats bzip2: 9/9**

**Speed**: 1.34s on bash (1.4MB) = 1.04 MB/s | 7z: 0.133s = 10.9 MB/s | **~10x slower** (was 11x)

## Architecture Summary

- **9-predictor CM** (o0, o1, o2, o4, o8, match, run-length, stride, lz-match-byte)
- **2-context logistic mixer** (separate weights for high-nibble vs low-nibble bit positions)
- **Arithmetic coder** for entropy coding
- **4MB BT4 match finder** with depth 48
- **Optimal parser** (forward DP, 4M window, 16 matches/position) with full rep-state tracking
- **Adaptive length pricing** — bt_price(len_tree) for accurate per-length costs
- **Split length trees** — separate 9-bit trees for rep-matches vs new-distance matches
- **CM-based literal pricing** — dedicated pricing CM gives DP accurate per-position literal costs
- **512K O4/O8 hash tables** for better binary prediction
- **BCJ filter** (x86 E8/E9) tried automatically, used when it helps
- **Enhanced BCJ filter** — RIP-relative LEA/MOV/store (x86-64 addressing modes)
- **Method 11: Hybrid ELF split** — code(BCJ) + stride sections separate + rest combined
- **Method 12: .eh_frame stream separation** — splits DWARF CFI into delta-encoded streams
- **Method 13: Content-type separation** — splits mixed sections into string/binary sub-streams, each compressed independently (enables stride detection on binary tables)
- **4MB block size** (CLI default)

## Key Files

- `src/block.c` — Main compression: CM, optimal parser, encode/decode, all methods
- `src/lz.c` / `src/lz.h` — 4MB BT4 match finder
- `src/predictors.c` — 8 predictors (o0, o1, o2, o4, o8, match, RL, stride)
- `src/mixer.c` — Logistic mixer
- `src/arithmetic.c` — Arithmetic coder
- `src/bcj.c` — x86 E8/E9 filter
- `src/file.c` — File-level API (blocking, CRC)
- `src/krv.h` — All type definitions
- `krv` — Python CLI wrapper
- `tests/test_roundtrip.py` — 19 unit tests
- `/tmp/compare3.sh` — Benchmark script

## Build & Test

```bash
cd /home/kerv/compression
rm -rf build && make
python3 tests/test_roundtrip.py
bash /tmp/compare3.sh
```

## Why We Still Lose on libc (0.57% gap)

### What Phase 17 Fixed (from Phase 16's 1.2% gap)

1. **Method 13: Content-type separation** — classifies 64-byte blocks as string (>56/64 printable) vs binary, compresses each sub-stream independently via `krv_compress_block`. The binary sub-stream gets stride detection (method 4), saving ~2.5KB on .rodata's lookup tables.
2. **Section size threshold** — skip sections < 4096 bytes in method 10. Small sections have poor compression due to CM cold-start and add 12 bytes of header each. Letting them fall into the combined gaps buffer saves ~500 bytes on libc.

### Remaining Root Cause (libc only)

The 0.57% gap (4,234 bytes) is primarily in:
- **.rodata string portion** (~1.4KB): Our CM converges slightly slower than LZMA on null-terminated string tables (44% null bytes as separators)
- **.gnu.hash** (~277 bytes): Bloom filter + hash chains are near-random data
- **Method 10 structural overhead**: Per-section compression loses cross-section LZ matches that 7z's single-stream approach gets

The gap is now fundamentally structural — 7z compresses the whole file as one LZMA stream and benefits from cross-section dictionary matches. Our per-section approach wins big on .eh_frame (-22%) and .text (-3%) but pays overhead for the section table and loses cross-section matches on small data sections.

---

## Primary Goal: Match 7z Speed While Beating (or Matching) Its Compression

Current state: **~11x slower** than 7z (was 35x → 130x), beats ratio on 8/9 files.

Target: **≤2x slower** than 7z with ratio still beating or matching 7z on most files. Willing to sacrifice 1-3% ratio for speed.

### Speed Optimizations Applied (Phase 19)

| Optimization | Speedup | Ratio Impact |
|-------------|---------|--------------|
| Stride detection: 256→all strides, 512→256 samples, interval 1024→2048, early exit >80% | ~1.15x | < 0.05% |
| Eliminate lz_advance tree inserts in emit pass (tree already built in Pass 1) | ~1.05x | zero |
| Skip method 7 (whole-file BCJ+LZ+CM) for large ELF > 256KB | ~1.5x | zero |
| Skip method 11 (hybrid split) for large ELF > 256KB | ~1.2x | zero |
| Parallel section compression (code + data sections via pthreads) | ~1.3x | zero |
| Mixer: cache stretched predictions + sum between predict/update | ~1.05x | zero |
| **Combined** | **3.4x** | **< 0.1%** |

### Speed Optimizations Applied (Phase 20)

| Optimization | Speedup | Ratio Impact |
|-------------|---------|--------------|
| Fast multiply-shift hash for O4 (single 32-bit load vs FNV byte loop) | ~1.03x | zero |
| O8 reduced to 6-byte context (better hash distribution for 512K table) | ~1.01x | **improved** (fewer collisions) |
| Cached hash index between predict/update (eliminates double hash computation) | ~1.03x | zero |
| Batch cm_sync for matched bytes (defers stride detection during matches) | ~1.02x | zero |
| 8-byte-at-a-time comparison in BT4 and rep-match scanning | ~1.01x | zero |
| Skip inactive predictor calls (stride/RL/match when not active) | ~1.01x | zero |
| Optimized pred_match_update_byte (single hash, lookup-before-insert) | ~1.01x | negligible |
| Direct stretch/squash table access in mixer (eliminate helper functions) | ~1.01x | zero |
| **Combined** | **~1.08x** | **improved on 7/9 files** |

### Speed Optimizations Applied (Phase 18)

| Optimization | Speedup | Ratio Impact |
|-------------|---------|--------------|
| Replace CM pricing with order-2 frequency estimator (blocks > 4KB) | ~1.5x | -0.1% on binaries, +5% on json/source |
| Strategic match length enumeration (16 lengths vs all) | ~1.7x | negligible (+0.01%) |
| Skip redundant method trials for ELF files | ~1.3x | zero |
| Reduce max matches per position (16 → 8) | ~1.05x | negligible |
| -O3 compiler optimization | ~1.1x | zero |
| **Combined** | **3.6x** | **< 0.3% on most files** |

### Remaining Speed Bottleneck (after Phase 19)

| Component | Cost | Why |
|-----------|------|-----|
| CM encoding pass (9 predictors per literal byte) | ~50% | Unavoidable for compression quality |
| BT4 match finder (tree insert + search per byte) | ~15% | Tree operations at every position |
| Mixer predict + update | ~13% | Dot product + weight update per bit |
| O4/O8 hash predictors | ~11% | Hash table lookups per bit |
| DP forward pass | ~5% | Already optimized with strategic lengths |
| Stride detection | ~3% | Reduced from 16% via optimization |

### Further Speed Optimization Roadmap

**Phase F: Reduce predictor count for binary data (expected: 1.3-1.5x)**
- Binary data (ELF .text) doesn't benefit much from stride/RL predictors
- Could use 7-predictor CM for code sections (skip stride + RL)
- Expected ratio loss: 0.1-0.5% on binaries

**Phase G: SIMD mixer (expected: 1.1x)**
- Vectorize the 9-element dot product in mixer_predict with SSE/AVX
- Small loop so benefit is marginal

**Phase H: Reduce O8 to O6 (expected: 1.05x)**
- O8 predictor uses 8-byte context hash — O6 might be sufficient
- Saves hash computation time

**Current: ~11x slower than 7z**
**Theoretical minimum: ~5-7x (CM encoding is fundamentally more expensive than LZMA)**

The fundamental limit is the 9-predictor CM encoding pass. To match 7z speed, we'd need to replace the CM with a simpler model (like LZMA's), which would sacrifice 3-5% compression ratio.

---

## Recommended Next Steps (in priority order)

### Tier 1: High confidence, moderate effort

**1. ELF-aware section splitting (DONE — Phase 14, gave 1.2-2.7%)**

**2. Enhanced BCJ with RIP-relative (DONE — Phase 15, gave 1.8-6.1%)**

**3. Hybrid ELF split for small binaries (DONE — Phase 15, gave 0.5% on ls)**

**4. Targeted .eh_frame preprocessing (DONE — Phase 16, gave 1.9% on libc, 1.4% on git)**

**5. Extended BCJ patterns (DONE — Phase 16, gave 0.1-0.5% on binaries)**

**6. .rodata content-type separation (DONE — Phase 17, gave 0.3% on libc)**

**7. Section size threshold optimization (DONE — Phase 17, gave 0.6% on libc)**

**8. Speed optimization (DONE — Phase 18, 3.6x faster with < 0.3% ratio loss)**

**9. Speed optimization II (DONE — Phase 19, 3.4x faster with < 0.1% ratio loss, now 11x vs 7z)**

**10. Larger block size for files > 4MB (expected: 1-3% on very large files)**

For files larger than 4MB, use 8MB or 16MB blocks. Currently git (4MB) fits in one block, but larger binaries would benefit from longer dictionary reach.

*Implementation:* Already supported by format. Just need auto-detection. ~20 lines.

### Tier 2: Medium confidence, higher effort

**9. Multi-block dictionary carry-forward (expected: 1-2% on large files)**

Carry LZ dictionary and CM state across block boundaries for files > 4MB.

**10. Instruction-aware BCJ with linear disassembly (REJECTED)**

Tested FF 15/25/35 (indirect CALL/JMP) and 0F B6/B7 (MOVZX) patterns. While they have 83-90% plausibility on libc, they cause regressions on bash (+106) and git (+213) due to false positives from sequential scanning. Without proper instruction length decoding (which requires a full x86 disassembler), additional BCJ patterns beyond what we have are not safe.

### Tier 3: Speculative

**11. Cross-section dictionary sharing** — Instead of fully independent per-section compression, share the LZ dictionary across sections while keeping separate CM instances. Would recover cross-section matches that 7z gets. Complex to implement correctly.

**12. Learned mixer weights** — Pre-train mixer weights on representative data, use as starting point.

**13. ANS (Asymmetric Numeral Systems)** — Replace arithmetic coder with ANS for ~1-2% better entropy coding efficiency.

**14. Neural network mixer** — Replace logistic mixer with small neural net for better probability estimation.

---

## What's Been Definitively Ruled Out

These approaches have been tested and confirmed to not help (or actively hurt):
- Adaptive mixer learning rate (any variant)
- More mixer contexts (4, 8, or position-dependent)
- SSE/APM secondary estimation
- Stronger/adaptive match-byte predictor probabilities
- Higher/lower count saturation limits
- Larger O2 hash table
- Two-pass DP with literal discount
- Enhanced BCJ with conditional jumps (without section awareness)
- Enhanced BCJ with conditional jumps (0F 80-8F) even with section awareness (false positives in instruction stream)
- Non-REX RIP-relative patterns (03, 3B, etc.) — 60%+ false positive rate without instruction length decoding
- BCJ patterns FF 15/25/35 (indirect CALL/JMP) — 83% plausible on libc but causes regressions on bash/git
- BCJ patterns 0F B6/B7 (MOVZX [rip]) — only 51% plausible on bash, too many false positives
- .rodata zero/non-zero split (CM already handles zeros via run-length predictor)
- 32+ matches per position
- Order-3 predictor (dilutes mixer)
- Post-match literal discount (hurts json)
- Match-byte XOR encoding (CM already handles this better)
- Context-dependent dist_align (splits training data, no benefit)
- Full CM sync on matched bytes (pollutes literal predictions)
- O0-only sync on matched bytes (helps binaries, hurts text/json)
- Position-state length trees (helps binaries, hurts text/json)
- Short-rep / 1-byte rep0 match (CM match-byte predictor is better)
- Larger BT4 hash table (no collisions to reduce)
- Delta filter on ELF binaries (destroys LZ match structure)
- Nibble-level O0 context (redundant — bit-tree ctx already encodes high nibble)
- ELF reorder as single stream (CM pollution outweighs LZ dictionary benefit)
- Order-4 hash estimator for DP pricing (64K table has too many collisions, worse than order-2)
- Reduced DP window (256K) for speed — barely helps speed, hurts compression on large binaries
- Reduced BT4 depth (24 vs 48) — negligible speed difference
- -march=native — no measurable improvement
- LTO (Link-Time Optimization) — no measurable speed improvement
- Unity build (single translation unit) — no improvement over separate compilation with -O3
- Limited stride set (20 specific strides) — misses stride 75 in json, causes 24% regression
- 128 samples for stride detection — insufficient to detect stride 75 reliably

---


## Phase 20 — Speed Optimization III (1.08x Faster, 10x vs 7z)

### What Changed
1. **Fast multiply-shift hash for O4/O8** — Replaced byte-by-byte FNV hash loop with single word-load + multiply-shift. O4 uses 32-bit load, O8 uses 6-byte context (4-byte + 2-byte loads). The 6-byte O8 hash actually improves compression because it has fewer collisions in the 512K table than the full 8-byte FNV hash.
2. **Cached hash index** — O2/O4/O8 predictors now cache the hash index from predict() and reuse it in update(), eliminating the double hash computation per bit.
3. **Batch cm_sync for matched bytes** — During the emit pass, matched bytes are synced in bulk with stride detection deferred until after the match. This avoids triggering expensive autocorrelation detection mid-match.
4. **8-byte-at-a-time comparison** — BT4 match finder and DP rep-match scanning use 64-bit word comparison with `__builtin_ctzll` for mismatch detection.
5. **Skip inactive predictors** — Stride, RL, and match predictors are only called when they have active state (stride detected, in a run, or match found). Otherwise PROB_HALF is used directly.
6. **Optimized pred_match_update_byte** — Reduced from two hash computations to one by looking up before inserting (same hash = same 3-byte context).
7. **Direct table access in mixer** — Stretch and squash tables accessed directly without wrapper functions.

### Speed Results
- bash (1.4MB): 1.41s → 1.34s (**1.05x faster**)
- git (4MB): 3.95s → 3.74s (**1.06x faster**)
- Now **~10x slower** than 7z (was 11x)
- Throughput: 1.04 MB/s (was 0.98 MB/s)

### Compression Impact
- **Improved** on 7/9 files (O6 hash has better distribution)
- services: -97 bytes, json: -139 bytes, git: -1041 bytes
- logs: +188 bytes (only regression > 0.1%)
- Still beats 7z on 8/9 files

### What Was Tried and Didn't Help
- **LTO (Link-Time Optimization)** — no improvement (same as Phase 19)
- **Merged lit-cost + match-collection loops** — worse cache behavior, 1.5% slower
- **Reduced strategic lengths (16→10)** — hurts json by 1.2%
- **Reduced max matches (8→4)** — hurts logs by 6.3%
- **Mixer skip for PROB_HALF predictions** — branch misprediction costs more than the multiply it saves
- **Reduced BT4 depth (48→32)** — negligible speed difference (confirmed again)

### Profile After Optimization
| Component | Before | After |
|-----------|--------|-------|
| pred_o4_predict | 6.81% | 3.05% |
| pred_o4_update | 2.62% | 1.22% |
| pred_o8_predict | 5.24% | 4.88% |
| pred_o8_update | 3.66% | 1.83% |
| pred_stride_update | 3.66% | 2.44% |
| mixer_predict | 7.85% | 7.32% |

---

## Phase 19 — Speed Optimization II (3.4x Faster, 11x vs 7z)

### What Changed
1. **Optimized stride detection** — Reduced autocorrelation samples from 512 to 256 (still detects stride 75 in json). Increased detection interval from 1024 to 2048 bytes. Added early exit when score > 80%. Reduced from 16.4% of runtime to 2.5%.
2. **Eliminated redundant tree inserts** — Added `lz_advance_skip()` for the emit pass. Since Pass 1 already builds the full BT4 tree, the emit pass only needs to advance `lz->pos` without re-inserting positions. Eliminated 5.2% of runtime.
3. **Skip method 7 for large ELF** — For ELF files > 256KB, method 10 (per-section split) always wins over method 7 (whole-file BCJ+LZ+CM). Skipping method 7 avoids a full `compress_m1` pass on the entire file.
4. **Skip method 11 for large ELF** — Method 11 (hybrid split) only wins for small binaries (ls). For large ELF, method 10 always wins.
5. **Parallel section compression** — Code section + all data sections compressed in parallel using pthreads. Each section gets its own output buffer, results assembled sequentially after all threads complete.
6. **Mixer predict/update fusion** — Cache stretched predictions and sum from `mixer_predict` for reuse in `mixer_update`, eliminating redundant stretch table lookups and dot product computation.

### Speed Results
- bash (1.4MB): 4.8s → 1.41s (**3.4x faster**)
- git (4MB): ~8.9s → 3.95s (**2.3x faster**)
- libc (2.1MB): ~5s → 1.94s (**2.6x faster**)
- Now **~11x slower** than 7z (was 35x)
- Throughput: ~1.0 MB/s (was 0.29 MB/s)

### Compression Impact
- All files within 0.1% of Phase 18 results
- Still beats 7z on 8/9 files

### What Was Tried and Didn't Help
- **LTO (Link-Time Optimization)** — no measurable improvement, compiler already optimizes well within each TU
- **Unity build (single translation unit)** — same as LTO, no improvement
- **Limited stride set (20 specific strides)** — missed stride 75 in json, caused 24% regression on json
- **128 samples for stride detection** — insufficient to reliably detect stride 75 in json
- **Stride interval 4096** — same regression as limited stride set (detection too infrequent)
- **-march=native** — no measurable improvement (already tested in Phase 18)

### Key Insight
The biggest single win was **skipping method 7 for large ELF** (~1.5x). For a 1.4MB ELF, method 7 runs `compress_m1` on the entire file (~3s), but method 10 always produces better results by compressing sections independently. The parallel section compression then overlaps the code section with data sections, saving another ~30% wall time.

The remaining ~11x gap vs 7z is fundamentally due to our 9-predictor CM encoding pass. Each literal byte requires 8 × (9 predictions + mixer + arithmetic coding) = ~80 operations. LZMA uses a simpler model (~20 operations per byte) at the cost of 3-5% worse compression. This is the core quality-vs-speed tradeoff.

---

## Phase 13 — ELF-Aware BCJ + Exploration
## Phase 18 — Speed Optimization (3.6x Faster)

### What Changed
1. **Fast order-2 frequency estimator** — Replaced full CM pricing pass (9 predictors + mixer per byte) with a lightweight order-2/order-1 frequency table for literal cost estimation in the DP. Only blocks ≤ 4KB still use full CM pricing (for .eh_frame sub-streams where accuracy matters).
2. **Strategic match length enumeration** — Instead of trying all lengths from min to max (up to 271 iterations per match), only try 16 strategic lengths: {2,3,4,5,6,8,12,16,24,32,48,64,96,128,192,273}. This covers the cost curve adequately while reducing DP iterations by 10-20x.
3. **Skip redundant ELF method trials** — For ELF files > 64KB, skip method 1 (plain LZ+CM) since section splits always win. Skip method 3 (basic BCJ) when ELF-BCJ (method 7) is available since it's strictly better.
4. **Reduced max matches per position** — From 16 to 8. The DP rarely benefits from more than 4-5 matches per position.
5. **-O3 compiler optimization** — Free 10% speedup.

### Speed Results
- bash (1.4MB): 17.5s → 4.8s (**3.6x faster**)
- Now 35x slower than 7z (was 130x)
- Throughput: 0.29 MB/s (was 0.08 MB/s)

### Compression Impact
- Binaries: < 0.3% regression (negligible)
- Logs: lost massive 23.7% advantage, now only 1.4% better than 7z (order-2 can't capture deep context patterns that CM exploits)
- JSON: **improved** from -30.8% to -33.7% vs 7z (order-2 estimator gives DP better local decisions)
- Source: **improved** from -3.2% to -23.8% vs 7z (same reason)
- Still beats 7z on 8/9 files

### What Was Tried and Didn't Help
- **Order-4 hash estimator** (64K table) — too many hash collisions, gave worse results than order-2
- **Reduced DP window (256K)** — barely helped speed (BT4 is the bottleneck, not DP) but hurt compression on large binaries
- **Reduced BT4 depth (24 vs 48)** — negligible speed difference (tree traversal isn't the bottleneck)
- **-march=native** — no measurable improvement

### Key Insight
The CM pricing pass was ~35% of total time (not 50% as estimated). The remaining time is split between BT4 match finder (~25%), CM encoding pass (~60%), and DP (~10%). The CM encoding pass is the fundamental bottleneck — it's what gives us superior compression but can't be eliminated without sacrificing quality. The 35x gap vs 7z is mostly due to our 9-predictor CM vs LZMA's simpler model.

---

## Phase 17 — Content-Type Separation + Section Threshold

### What Changed
1. **Method 13: Content-type separation** — Classifies 64-byte blocks by printable byte density (threshold: >56/64). Splits data into string and binary sub-streams, each compressed independently via `krv_compress_block`. The binary sub-stream benefits from stride detection (method 4) on lookup tables that were previously hidden within the heterogeneous .rodata section.

2. **Section size threshold in method 10** — Skip sections smaller than 4096 bytes. Small sections suffer from CM cold-start overhead and add 12 bytes of header each. Letting them fall into the combined gaps buffer (compressed as one stream) is more efficient.

### Improvement vs Phase 16
- libc: 752,323 → 747,636 (-4,687, -0.6%) — gap reduced from 1.2% to 0.57%
- ls: 52,134 → 51,789 (-345, -0.7%)
- bash: 514,098 → 513,865 (-233, -0.05%)
- git: 1,491,753 → 1,491,524 (-229, -0.02%)

### Key Insight
The biggest win came from using `krv_compress_block` (instead of `compress_m1`) for method 13's sub-streams. This enables stride detection on the binary portion of .rodata, which contains lookup tables with regular structure (stride-8 patterns from paired 32-bit values). The stride transpose alone saves ~2.5KB on libc's .rodata binary tables.

The section size threshold is a simple but effective optimization: small sections (<4096 bytes) compress poorly individually due to CM cold-start, and their 12-byte header entries add up. Combining them into the gaps buffer gives better LZ matches and eliminates per-section overhead.

### What Was Tried and Didn't Help
- **BCJ patterns FF 15/25/35** (indirect CALL/JMP [rip+disp32]) — 90% plausible on libc but causes +106 bytes regression on bash due to false positives from sequential scanning
- **BCJ patterns 0F B6/B7** (MOVZX [rip+disp32]) — only 51% plausible on bash, too many false positives
- **Content-type threshold 48** (75% printable) — threshold 56 (87.5%) gives 502 bytes better results on libc
- **Block sizes 32 or 128** for content classification — 64 bytes is optimal (32 has too much bitmap overhead, 128 has worse separation)
- **Section threshold 8192** — too aggressive, loses stride detection on medium sections (ls regresses)

---

## Phase 16 — .eh_frame Transform + Extended BCJ

### What Changed
1. **Method 12: .eh_frame stream separation** — Detects DWARF CFI sections (CIE+FDE records) and separates them into independent streams:
   - Lengths (16-bit per record, highly repetitive — only 23 unique values in libc)
   - PC-begin deltas (delta-encoded, much more compressible than raw relative offsets)
   - PC-ranges (function sizes)
   - CIE references (which CIE each FDE points to — nearly all the same)
   - Instruction bodies (DWARF CFI opcodes + padding)
   - Handles multiple CIEs correctly (libc has 3)
   - Null terminator preserved

2. **Extended BCJ patterns** — Added safe REX-prefixed RIP-relative patterns:
   - REX+C7 (MOV [rip+disp32], imm32) — 181 in libc, 0 false positives, 10-byte instruction
   - REX+3B (CMP r64, [rip+disp32]) — 50 in libc, 0 false positives
   - REX+39 (CMP [rip+disp32], r64) — 27 in libc, 0 false positives

### Improvement vs Phase 15
- libc: 767,381 → 752,323 (-15,058, -2.0%) — gap reduced from 3.2% to 1.2%
- git: 1,513,976 → 1,491,753 (-22,223, -1.5%)
- bash: 525,149 → 514,098 (-11,051, -2.1%)
- ls: 52,372 → 52,134 (-238, -0.5%)

### Key Insight
The .eh_frame transform is the biggest single-section improvement since the enhanced BCJ filter. DWARF CFI has extremely predictable structure that our general-purpose CM cannot exploit efficiently. By separating the streams, each one becomes highly compressible:
- Lengths compress to 17.6% (only 23 unique values across 3,791 records)
- PC-begin deltas compress to 33.4% (vs raw relative offsets)
- CIE refs compress to nearly nothing (97% reference the same CIE)

The .eh_frame section now **beats** 7z by 22.3% (was 19.7% worse).

### Why libc Still Loses (1.2%)
- .rodata: 12.4% worse than 7z (heterogeneous: strings + tables + padding)
- The remaining gap is fundamental: our 9-predictor CM converges slower than LZMA on mixed binary data
- Splitting .rodata into string/binary regions saves ~2.2KB but adds complexity

## Phase 15 — Enhanced BCJ + Hybrid ELF Split

### What Changed
1. **Enhanced BCJ filter** — In addition to E8/E9 (CALL/JMP), now also converts RIP-relative addressing to absolute:
   - Non-REX LEA/MOV: `8D/8B [mod=00,rm=101] disp32` (6-byte instructions)
   - REX-prefixed LEA/MOV/store: `[40-4F] 8D/8B/89 [mod=00,rm=101] disp32` (7-byte instructions)
   - This covers ~48K additional instructions in git's .text (193KB of address data)
   - Very low false positive rate (pattern requires specific ModRM bits)

2. **Method 11: Hybrid ELF split** — For small ELF binaries where per-section split loses cross-section LZ matches:
   - .text compressed separately with BCJ (same as method 10)
   - Stride-detectable sections compressed individually (same as method 10)
   - All remaining sections combined into ONE stream (preserves cross-section dictionary)
   - Wins for ls (-245 bytes vs method 10), loses for larger binaries

### Improvement vs Phase 14
- ls: 53,313 → 52,372 (-941, -1.8%) — now beats 7z!
- bash: 558,982 → 525,149 (-33,833, -6.1%) — now beats 7z by 3.7%!
- git: 1,606,368 → 1,513,976 (-92,392, -5.8%) — now beats 7z by 2.4%!
- libc: 786,821 → 767,381 (-19,440, -2.5%) — gap reduced from 5.8% to 3.2%

### Key Insight
The enhanced BCJ filter is the single biggest improvement since the optimal parser. Standard BCJ (E8/E9 only) misses ~40% of the address bytes in x86-64 code. RIP-relative LEA/MOV instructions are extremely common in position-independent code (PIE/PIC) and their displacement fields contain addresses that become redundant after conversion to absolute form.

Conditional jumps (0F 80-8F) were tested but hurt larger binaries due to false positives from sequential scanning (the 2-byte prefix can appear as part of other instructions' immediate data).

### Why libc Still Loses (3.2%)
- .eh_frame: 19% worse than 7z (DWARF CFI, variable-length records, no good transform)
- .rodata: 12% worse (mixed content: jump tables + strings + locale data)
- libc has more hand-written SIMD assembly (fewer RIP-relative patterns)
- The CM's 9-predictor mixer converges slower than LZMA on heterogeneous binary data

### What Was Tried and Didn't Help
- **Conditional jump filtering (0F 80-8F)** — helps ls (-433) but hurts git (+14,520) due to false positives in instruction stream
- **Combined rest stream for large binaries** — worse than per-section split because stride detection on individual sections gives bigger wins

---

## Phase 14 — ELF Per-Section Split

### What Changed
1. **Method 8: ELF 2-way split** — separates code (BCJ + LZ+CM) from non-code (LZ+CM) into independent streams. Each gets its own CM instance.
2. **Method 10: ELF per-section split** — compresses each ELF section individually with `krv_compress_block` (which tries stride detection, BCJ, etc.). Gaps between sections are collected and compressed as a separate segment. Wins for larger binaries (bash, git, libc).

### Key Insight
Per-section compression enables stride detection on structured sections (.rela.dyn, .dynsym, .eh_frame_hdr) that would be missed when compressed as part of a larger mixed stream. KRV crushes 7z on .rela sections (-23%) thanks to stride detection.

### Per-Section Analysis (git)
- .text (code): KRV **beats** 7z by 5.8%
- .rela.dyn: KRV **beats** 7z by 23.2% (stride detection)
- .rodata: KRV 2.4% behind 7z
- .eh_frame: KRV 19% behind 7z (DWARF unwind, variable-length records)
- .data: KRV 9.1% behind 7z

### What Was Tried and Didn't Help
- **Method 9 (reorder as single stream)** — putting rest first then code in one stream. CM pollution from mixing data types outweighs LZ dictionary benefit.
- **Nibble-level O0 context** — redundant, the bit-tree context already encodes the high nibble.
- **Stride transpose on .eh_frame** — makes it worse (variable-length records don't align).
- **Merging small sections** — only 88 bytes savings, not worth complexity.

### Improvement vs Phase 13
- ls: 53,935 → 53,313 (-622, -1.2%)
- bash: 574,645 → 558,982 (-15,663, -2.7%)
- git: 1,629,191 → 1,606,368 (-22,823, -1.4%)
- libc: 803,383 → 786,821 (-16,562, -2.1%)

### Remaining Gap Analysis
The remaining 1.8-5.8% gap on binaries is primarily in:
1. **.eh_frame** (19% worse than 7z) — DWARF CFI with variable-length records, no good preprocessing transform found
2. **.data** (9% worse) — mixed initialized data
3. **.rodata** (2.4% worse) — strings/constants, close to optimal

---


### What Changed
1. **ELF-aware BCJ** (methods 6/7) — parses ELF section headers and only applies E8/E9 filter to sections with SHF_EXECINSTR flag. Avoids false positives in .rodata, .eh_frame, .data sections.

### Improvement vs Phase 12
- ls: 54,075 → 53,935 (-140, -0.3%)
- bash: 576,328 → 574,645 (-1,683, -0.3%)
- git: 1,632,710 → 1,629,191 (-3,519, -0.2%)
- libc: 806,036 → 803,383 (-2,653, -0.3%)

### What Was Tried and Didn't Help
- **Context-dependent dist_align** (4 groups by slot range) — splits training data 4 ways, negligible/slightly worse
- **Full CM sync on matched bytes** (update all predictors during cm_sync_byte) — catastrophically bad, pollutes CM predictions for literals
- **O0-only sync on matched bytes** — helps binaries slightly (-1159 git) but hurts logs (+1804) and json (+164)
- **Position-state length trees** (4 trees by pos&3) — helps large binaries (-888 bash, -634 git) but hurts text/json
- **Short-rep (1-byte rep0 match)** — worse everywhere because is_rep0_long bit adds overhead to every rep0 match, and CM's match-byte predictor already handles this case efficiently
- **Larger BT4 hash table** (22-bit vs 20-bit) — zero effect, no collisions being missed
- **Delta filter** (XOR-delta at stride 1/2/4/8 + ELF-BCJ) — never wins because delta destroys LZ match structure

### Key Insight
The remaining 3-8% gap on binaries is definitively confirmed to be in literal encoding efficiency. Every approach that adds model complexity (more contexts, more state) helps large binaries but hurts text/json due to reduced training data per context. The CM's 9-predictor mixer is fundamentally slower to converge than LZMA's simpler model. The only safe improvements are preprocessing transforms (like ELF-aware BCJ) that don't touch the CM.

---

## Phase 12 — 4M DP Window + Exploration

### What Changed
1. **4M DP window** (was 2M) — full-block optimal parsing for files ≤ 4MB

### What Was Tried and Didn't Help
- **Adaptive mixer learning rate** — fast early (shift 14-15), slow later (shift 17). Hurts because 2 mixer contexts each get millions of updates; the fixed shift 16 is already optimal.
- **8 mixer contexts (per bit position)** — helps text (copyright -127, source -250) but hurts binaries (+4875 bash, +16284 git). More contexts = less training data per context.
- **4 mixer contexts (per 2 bit positions)** — same pattern, less extreme.
- **Position-dependent mixer context (nibble × posState)** — helps some binaries (ls -163, libc -472) but hurts json (+569) and git (+758). Binary data has positional structure but text doesn't.
- **Stronger match-byte predictor (7/8 vs 3/4)** — helps logs/json but hurts binaries because wrong predictions are penalized more severely.
- **Adaptive match-byte predictor** — tracks hit rate and adjusts probability. Hurts binaries because it starts at 50% and converges slowly.
- **SSE (Secondary Symbol Estimation)** — catastrophically bad. Our logistic mixer already produces well-calibrated probabilities; SSE adds noise during learning.
- **Higher count saturation (2000 vs 1000)** — slower adaptation hurts everything.
- **Faster O4/O8 adaptation (255 or 500 saturation)** — helps binaries slightly but hurts text/json significantly.
- **Larger O2 hash (256K vs 64K)** — no effect (O2 context space is only 64K, no collisions to reduce).
- **32 matches per position** — negligible over 16.
- **Two-pass DP with post-match literal discount** — negligible effect (pricing CM is already accurate enough).
- **Enhanced BCJ (conditional jumps 0F 80-8F)** — helps ls (-453) but hurts git (+13264) due to false positives in data sections.

### Key Insight
The remaining 3-8% gap on binaries is in literal encoding efficiency (0.16-0.24 bits/byte). Our 9-predictor mixer is fundamentally slower to converge than LZMA's simpler model. Every CM-level tweak that helps binaries hurts text/json and vice versa. The only safe improvement is better match selection (larger DP window), which doesn't affect literal encoding.

### Improvement vs Phase 11
- git: 1,640,479 → 1,632,710 (-7,769, -0.5%) — from 4M DP window

---

## Phase 11 — Adaptive Pricing + Split Trees + Contextualized Mixer + Larger Window

### What Changed (from Phase 10)
1. **Adaptive length pricing** — replaced flat 9×8=72 with `bt_price(lm->len_tree, 9, l-LZ_MIN_MATCH)` and `bt_price(lm->rep_len_tree, 9, l-LZ_MIN_MATCH)` for accurate per-length costs
2. **Split rep/dist length trees** — separate 9-bit trees for rep-matches vs new-distance matches
3. **2-context logistic mixer** — separate weight sets for high-nibble (bits 7-4) vs low-nibble (bits 3-0) bit positions
4. **512K O4/O8 hash tables** — increased from 256K for better binary context prediction
5. **2M DP window** — increased from 512K for globally better match/literal decisions

### Improvement vs Phase 10
- services: 5,048 → 4,978 (-70, -1.4%)
- copyright: 23,490 → 23,198 (-292, -1.2%)
- ls: 54,620 → 54,075 (-545, -1.0%)
- bash: 584,201 → 576,328 (-7,873, -1.3%)
- git: 1,671,926 → 1,640,479 (-31,447, -1.9%)
- libc: 815,108 → 805,981 (-9,127, -1.1%)
- logs: 22,211 → 19,121 (-3,090, -13.9%)
- json: 4,541 → 4,538 (-3, -0.1%)
- source: 1,118 → 1,311 (+193, +17.3% — from 2-ctx mixer slower adaptation on small files)

---

## Phase 10 — CM-Based Pricing + Large DP Window

### What Changed
1. **Removed dual-mode CM** (shallow CM, meta-mixer, context reset) — added complexity for only 1.5% gain
2. **CM-based literal pricing** — dedicated pricing CM runs through all bytes, gives DP accurate per-position literal costs instead of crude order-0 frequency estimate
3. **512K DP window** (was 16K) — larger lookahead for globally better match/literal decisions. Only works because CM pricing is accurate over long spans (old estimator broke down past 16K)

### Current Results

| File | Original | 7z | **KRV** | vs 7z |
|------|----------|-----|---------|-------|
| services | 12,813 | 5,241 | **5,048** | **-3.7%** ✓ |
| copyright | 110,653 | 23,351 | **23,490** | +0.6% |
| ls | 142,312 | 52,383 | **54,620** | +4.3% |
| bash | 1,446,024 | 545,073 | **584,201** | +7.2% |
| git | 4,066,232 | 1,550,976 | **1,671,926** | +7.8% |
| libc | 2,125,328 | 743,402 | **815,108** | +9.6% |
| logs | 832,184 | 25,074 | **22,211** | **-11.4%** ✓ |
| json | 165,670 | 6,563 | **4,541** | **-30.8%** ✓ |
| source | 22,725 | 1,355 | **1,118** | **-17.5%** ✓ |

**Beats 7z: 4/9** | copyright within 0.6% | Binary gap: 7-10% (was 10-12%)

### What We Learned
- XOR-delta matched-literal encoding does NOT help — our 9-predictor CM already handles post-match literals better than any simple bit-tree
- Dual-mode CM (shallow + meta-mixer) adds complexity for marginal gain — the CM is already good enough
- The **DP window size** is the single biggest lever for binaries — going from 16K to 512K improved git by 66K
- **CM-based pricing** is the key enabler — it makes large windows work (old estimator broke down past 16K)
- Full-block DP (4MB window) gives another 30K on git but costs 600MB+ RAM

---

