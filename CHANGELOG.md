# KRV Compression - Development Log

## Phase 18 — Speed Optimization (Complete)
- Replaced full CM pricing pass with fast order-2/order-1 frequency estimator for blocks > 4KB
  - Order-2: 64K hash table with 256 byte counts per context
  - Falls back to order-1 (256 full contexts) when order-2 has insufficient data
  - Full CM pricing retained only for blocks ≤ 4KB (tiny .eh_frame sub-streams)
  - Eliminates ~35% of total compression time
- Strategic match length enumeration in DP
  - Instead of trying all lengths min..max (up to 271 per match), try 16 strategic lengths
  - Lengths: {2,3,4,5,6,8,12,16,24,32,48,64,96,128,192,273}
  - Covers the cost curve adequately, reduces DP iterations 10-20x
  - Negligible compression impact (+0.01%)
- Skip redundant method trials for ELF files
  - Skip method 1 (plain LZ+CM) for ELF > 64KB — section splits always win
  - Skip method 3 (basic BCJ) when ELF-BCJ (method 7) available — strictly better
  - Saves 1-2 full compress_m1 calls on ELF files
- Reduced max matches per position from 16 to 8
- Switched to -O3 compiler optimization
- Result: **3.6x faster** (17.5s → 4.8s on bash). Still beats 7z on 8/9 files.
- Tradeoff: logs lost massive 23.7% advantage (now only 1.4% better than 7z). JSON/source improved.
- Tried and rejected: order-4 hash (too many collisions), reduced DP window (hurt binaries), reduced BT depth (no speed gain)

## Phase 17 — Content-Type Separation + Section Threshold (Complete)
- Method 13: Content-type separation for mixed data sections
  - Classifies 64-byte blocks by printable byte density (threshold: >56/64 = 87.5%)
  - Splits into string and binary sub-streams
  - Each sub-stream compressed via `krv_compress_block` (enables stride detection, BCJ, etc.)
  - Binary sub-stream of .rodata gets stride detection (method 4) on lookup tables
  - Saves ~2.5KB on libc's .rodata alone
- Section size threshold in method 10: skip sections < 4096 bytes
  - Small sections suffer CM cold-start and add 12 bytes header each
  - Combined in gaps buffer for better cross-section LZ matches
  - Saves ~500 bytes on libc
- Tested and rejected: BCJ patterns FF 15/25/35 and 0F B6/B7 (false positives on bash/git)
- Result: libc gap reduced from 1.2% to **0.57%**. ls now beats 7z by 1.1% (was 0.5%).

## Phase 16 — .eh_frame Transform + Extended BCJ (Complete)
- Method 12: .eh_frame stream separation — parses DWARF CFI (CIE/FDE records), separates into:
  - Lengths (16-bit, highly repetitive)
  - PC-begin deltas (delta-encoded relative offsets)
  - PC-ranges (function sizes)
  - CIE references (which CIE each FDE points to)
  - Instruction bodies (DWARF opcodes + padding)
  - Handles multiple CIEs, preserves null terminator
- Extended BCJ: added REX+C7 (MOV [rip+disp32],imm32), REX+3B (CMP r,[rip]), REX+39 (CMP [rip],r)
  - All have 0% false positive rate in non-code sections
  - REX+C7 uses correct 10-byte instruction length for RIP calculation
- Result: libc gap reduced from 3.2% to **1.2%**. bash -5.7%, git -3.8% vs 7z.
- .eh_frame now beats 7z by 22.3% (was 19.7% worse)

## Phase 15 — Enhanced BCJ + Hybrid ELF Split (Complete)
- Enhanced BCJ filter: converts RIP-relative LEA/MOV/store displacements to absolute
  - Non-REX: `8D/8B [mod=00,rm=101] disp32` (6-byte LEA/MOV instructions)
  - REX-prefixed: `[40-4F] 8D/8B/89 [mod=00,rm=101] disp32` (7-byte instructions)
  - Covers ~48K additional instructions in git (193KB of address data beyond E8/E9)
- Method 11: Hybrid ELF split for small binaries (code+BCJ separate, stride sections separate, rest combined)
- Conditional jumps (0F 80-8F) tested but rejected — false positives in instruction stream hurt large binaries
- Result: **Beats 7z on 8/9 files.** ls -1.8%, bash -6.1%, git -5.8%, libc -2.5% vs Phase 14.
- Now beats 7z on bash (-3.7%), git (-2.4%), ls (-0.02%). Only libc remains behind (+3.2%).

## Phase 14 — ELF Per-Section Split (Complete)
- Method 10: compresses each ELF section individually with krv_compress_block
- Each section gets its own CM instance + can use stride detection independently
- Stride detection on .rela.dyn gives -23% vs 7z
- Result: ls -1.2%, bash -2.7%, git -1.4%, libc -2.1% vs Phase 13

## Phase 13 — ELF-Aware BCJ (Complete)
- Methods 6/7: parse ELF section headers, only apply E8/E9 filter to SHF_EXECINSTR sections
- Avoids false positives in .rodata, .eh_frame, .data
- Result: 0.2-0.3% improvement on all binaries vs Phase 12

## Phase 12 — 4M DP Window + Exploration (Complete)
- Full-block optimal parsing (4MB DP window)
- Explored: adaptive mixer LR, 4/8 mixer contexts, position-dependent contexts, SSE, stronger match-byte
- All CM-level tweaks are zero-sum (help binaries ↔ hurt text/json)
- Result: git -0.5% from larger window. Confirmed CM is at local optimum.

## Phase 11 — Adaptive Pricing + Split Trees + 2-Context Mixer (Complete)
- Adaptive length pricing with bt_price() for accurate per-length costs
- Split rep/dist length trees (separate 9-bit trees)
- 2-context logistic mixer (high-nibble vs low-nibble bit positions)
- 512K O4/O8 hash tables
- 2M DP window
- Result: 1-2% improvement across all files

## Phase 10 — CM-Based Pricing + Large DP Window (Complete)
- Dedicated pricing CM for accurate per-position literal costs in DP
- 512K DP window (enabled by accurate pricing over long spans)
- Removed dual-mode CM (complexity for marginal gain)
- Result: Binary gap reduced from 10-12% to 7-10%

## Phase 8 — Matched-Literal Predictor + 4M Blocks (Complete)
- 9th CM predictor: uses last LZ match distance to predict next literal byte (match-byte context)
- Post-match literal discount: DP reduces literal cost by 20% for first literal after a match (CM predicts better there)
- OPT_WINDOW increased from 4096 to 16384 — longer optimal parsing decisions
- CLI default block size changed from 256K to 4M — eliminates cross-block losses
- Auto-stride detection + column-split transpose added (triggers on structured data files)
- Results: git -6.2K, libc -2.1K, bash -1.4K from post-match discount. Source now beats 7z by 21%!
- Now beats 7z on 4/9 files (services, json, source, copyright nearly tied)
- Binary gap reduced from 15-23% to 10-12%

## Phase 7 — Entropy-Gated Literal Pricing (Complete)
- Per-position literal cost estimation using order-0 byte frequency from recent context
- Gated application: only use estimate when it differs significantly from baseline (< 32 or > 56 in 1/8-bit units), otherwise use fixed baseline of 48
- Cold-start fix: seed from first 64 bytes of chunk when no prior context available
- Result: JSON 4,225 (beats 7z by 35.6%, beats zstd by 19%!). Logs improved to 29,994 (+19.6% vs 7z, was +28%). Small regressions on text/binaries (~0.5-1%).
- Rep-slot lookahead attempted but removed — DP's rep-state tracking already optimizes this naturally.

## Phase 6 — Optimal Parsing with Rep-State Tracking (Complete)
- Forward DP over 4096-byte windows with full rep-state (4 × uint32) at each node
- Three-pass architecture: (1) BT match collection, (2) forward DP, (3) backward trace + forward emit
- Adaptive distance pricing using actual bit-model probabilities (log2 lookup table)
- Flat length cost to prevent short-match preference that disrupts CM context
- Rep-match validation at emit time handles any state divergence
- Result: Beats 7z on services (-3.5%) and json (-2.5%). Binaries improved 1-2% over greedy.

## Phase 1 — Foundation (Complete)
- Built arithmetic coder (32-bit, byte-aligned renormalization)
- Implemented Order-0, Order-1, Order-2 predictors with frequency counting
- Logistic mixer with online gradient descent weight learning
- Block splitter + KRV file format (magic, CRC-32, block headers)
- Python CLI wrapper (`krv compress`, `krv decompress`, `krv info`)
- 19/19 round-trip tests passing
- Result: ~50% reduction on binaries, 90%+ on repetitive text

## Phase 2 — Full Predictor Suite (Complete)
- Added Order-4 (FNV hash of 4 prev bytes)
- Added Order-8 (FNV hash of 8 prev bytes)
- Added match finder predictor (64KB sliding window, 3-byte hash)
- Added run-length predictor (detects byte runs, confidence scales with length)
- Mixer expanded to 7 predictors
- Result: 70% improvement on source code, 81% on JSON vs Phase 1

## Phase 3 — Dynamic Model Spawning + Hybrid (Complete)
- Implemented dynamic model spawning (monitors prediction error, spawns specialized sub-models)
- Spawner detects periodicity in recent bytes, creates context-specific models
- Model retirement when weight drops (LRU by contribution)
- Added LZ77 with hash chains (64KB window) as alternative method
- **Hybrid approach:** tries pure CM and LZ+CM per block, picks smaller output
- Fixed critical bug: mixer stretch/squash tables weren't initialized when bypassing `mixer_init()`
- Fixed LZ offset overflow (65536 didn't fit in 16-bit encoding)
- Result: Beats gzip on all files, beats zstd on binaries

## Phase 4 — LZMA-Class LZ Engine (Complete)
- Rewrote LZ engine: 4MB window, binary tree match finder (BT4), 4-byte hashing
- Added rep-match support (4 recent distances, very cheap to encode)
- Distance slot encoding (log2-based, like LZMA)
- Adaptive bit models for all LZ token types
- Greedy parsing with rep-match preference

### What we tried that didn't work:
- **LZMA-style state machine (12 states):** Implemented full encoder but had an unresolved encode/decode desync bug on files >118KB. The state transitions and matched-literal context caused divergence. Abandoned in favor of simpler approach.
- **16MB window:** Caused segfaults due to 128MB tree allocation. Reverted to 4MB.
- **256 literal contexts (full prev_byte):** Too many contexts, models couldn't adapt fast enough on small files. Worse than 16 contexts.
- **64 literal contexts (prev_byte >> 2):** Slightly worse than 16 on small text.
- **32 literal contexts (prev_byte >> 3):** Slightly worse than 16 on small text.
- **4 position bits:** Hurt small files (more contexts = slower adaptation). 2 bits was optimal.
- **Adaptation rate 5 (LZMA default):** Rate 4 was measurably better across all file types.
- **Adaptation rate 3:** Better on tiny files, worse on medium. Rate 4 is the sweet spot.
- **Prediction-guided LZ (Phase 3 attempt):** LZ token encoding overhead exceeded savings from matches on structured data. Pure CM was better for text; LZ only helps on binaries.

### What worked well:
- **Hybrid method selection** (pure CM vs LZ+CM per block) — lets each method shine where it's best
- **Rep-matches** — extremely cheap (2-3 bits) vs new match (22+ bits for distance)
- **Binary tree match finder** — finds longer matches than hash chains
- **Adaptive bit models with rate 4** — faster adaptation than LZMA's rate 5
- **Context mixer for literals** — 8 predictors with learned weights beats fixed-context models
- **Incompressible block passthrough** — graceful handling of random/compressed data

## Phase 5 — Novel Improvements (Complete)

### Implemented (helped):
- **Stride predictor (Structural Resonance Detection):** Autocorrelation-based stride detection (strides 2-256, re-detects every 1024 bytes, 64KB buffer). Predicts `byte[pos] = byte[pos - stride]`. Source -18%, JSON -11%, logs -6%.
- **Lazy matching:** Defers non-rep match if pos+1 has a longer rep-match. Logs -14%, binaries -1-2%.
- **BCJ filter (E8/E9 x86 transform):** Unconditional CALL/JMP relative→absolute. Auto-detects x86. ls -5%, libc -1%.
- **Context-modeled distance bits:** Adaptive encoding for low distance bits. Binaries -0.3-0.5%.
- **Match cost rejection:** Rejects new-distance matches where cost > 7×len. Binaries -1%.
- **Length-before-distance encoding order.**

### Tried but didn't help:
- **SSE/APM:** Mixer already well-calibrated. Made everything worse.
- **Per-bit-position mixers:** Helped text, hurt binaries. Net negative.
- **Full predictor sync during LZ matches:** Created predictor/mixer mismatch.
- **Larger BT search depth (96):** No improvement.
- **Larger CM match window (256KB):** No improvement.
- **Length-dependent distance slots:** Marginal, not enough training data.
- **Pure price-based match selection:** Helped binaries 2-4%, catastrophically hurt text/JSON.
- **Near-optimal parsing (forward DP):** Helped JSON dramatically (-26%, beat 7z!) but hurt binaries and source code. Root cause: rep-state divergence between planning and emit phases, crude price model doesn't account for CM disruption. Would need multi-week effort to implement correctly with proper rep-state tracking through DP nodes.

## Current Status

**Beats:** gzip (9/9), zstd (8/9), bzip2 (7/9)  
**Behind:** xz (8/9), 7z (8/9) — gap is 3-27% (was 7-43% in Phase 4)

## Remaining Ideas (Not Yet Tried)

- Optimal/near-optimal parsing with forward DP (biggest remaining gap)
- Larger window with memory-mapped tree
- Word-level modeling for text
- Delta coding for structured binary
- Parallel block compression
