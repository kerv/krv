# KRV Lossless Compression Algorithm

## Overview

KRV is an experimental lossless compression algorithm based on **adaptive context mixing with online learning and dynamic model spawning**. It combines multiple prediction models whose outputs are mixed by a lightweight online learner, then encodes the result with arithmetic coding.

The novel contribution is a **hierarchical model spawning mechanism** — the system monitors prediction quality in real-time and dynamically creates specialized sub-models when existing models fail to predict the current data region, then retires them when they become stale.

**File extension:** `.krv`

---

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                   Python CLI Wrapper                  │
│         (argparse, file I/O, progress, validation)   │
└─────────────────────┬───────────────────────────────┘
                      │ ctypes / subprocess
┌─────────────────────▼───────────────────────────────┐
│                   C Core Engine                       │
│                                                      │
│  ┌─────────────┐  ┌──────────────┐  ┌───────────┐  │
│  │ Block Splitter│  │ Model Manager │  │ Arithmetic│  │
│  │             │  │  (spawner)    │  │   Coder   │  │
│  └──────┬──────┘  └──────┬───────┘  └─────┬─────┘  │
│         │                │                 │        │
│  ┌──────▼────────────────▼─────────────────▼─────┐  │
│  │              Prediction Pipeline               │  │
│  │                                                │  │
│  │  ┌─────────┐ ┌─────────┐ ┌─────────┐        │  │
│  │  │ Order-N │ │  Match  │ │   Run   │  ...    │  │
│  │  │ Markov  │ │ Finder  │ │ Length  │         │  │
│  │  └────┬────┘ └────┬────┘ └────┬────┘        │  │
│  │       └────────────┼───────────┘              │  │
│  │                    ▼                          │  │
│  │           ┌─────────────────┐                 │  │
│  │           │ Logistic Mixer  │                 │  │
│  │           │ (online learning)│                 │  │
│  │           └────────┬────────┘                 │  │
│  │                    ▼                          │  │
│  │           Combined Probability                │  │
│  └───────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────┘
```

---

## Core Algorithm

### 1. Block Processing

Input is split into blocks (default 256KB, configurable). Each block is compressed independently, enabling:
- Parallel decompression (future)
- Streaming capability (future)
- Memory-bounded operation for GB-scale files

Block header stores: original size, compressed size, model state snapshot flag.

### 2. Prediction Models (Base Predictors)

Each predictor outputs a probability `P(next_bit = 1)` given context. We operate at the **bit level** for maximum granularity.

| Model | Description | Context |
|-------|-------------|---------|
| Order-0 | Byte frequency counter | None (global stats) |
| Order-1 | Bigram model | Previous byte |
| Order-2 | Trigram model | Previous 2 bytes |
| Order-4 | 4-gram model | Previous 4 bytes |
| Order-8 | 8-gram model (sparse) | Previous 8 bytes (hashed) |
| Match | Finds longest match in recent history | Sliding window (64KB) |
| Run-Length | Detects repeated byte runs | Current run state |
| Bit-Run | Detects repeated bit patterns | Recent bit history |
| Word | Models word-level patterns (for text) | Word boundary detection |
| Sparse | Non-contiguous context bytes | Bytes at positions -1, -2, -4, -8 |

### 3. Context Mixing (The Mixer)

Predictions from all active models are combined using **logistic mixing**:

```
P_combined = σ(Σ wᵢ · stretch(Pᵢ))
```

Where:
- `stretch(p) = ln(p / (1-p))` (logit function)
- `σ(x) = 1 / (1 + e^(-x))` (sigmoid)
- `wᵢ` = learned weight for model i

Weights are updated online after each bit using gradient descent:
```
wᵢ += η · (actual_bit - P_combined) · stretch(Pᵢ)
```

Learning rate `η` decays per-block to stabilize predictions.

### 4. Dynamic Model Spawning (Novel Mechanism)

This is the experimental core of KRV. The Model Manager monitors prediction error rates over sliding windows:

**Spawn trigger:** When the combined prediction error exceeds a threshold over the last N bits (configurable, default N=1024), the system:
1. Analyzes the failing region to detect patterns (periodicity, structure)
2. Spawns a specialized sub-model tuned to the detected pattern
3. Adds it to the mixer with an initial weight

**Retirement trigger:** When a spawned model's contribution to the mix drops below a threshold (its weight decays toward zero), it is retired to free memory.

**Model types that can be spawned:**
- Periodic model (detected period length)
- Record model (fixed-width record structure)
- Delta model (differences between consecutive values)
- High-order context (order-12, order-16 for highly structured data)

**Memory budget:** Total spawned models are capped (default: 32 active). Least-useful models are evicted first (LRU by weight contribution).

### 5. Arithmetic Coder

Standard binary arithmetic coder with:
- 32-bit precision
- Renormalization on overflow
- End-of-stream marker

The arithmetic coder consumes `P_combined` for each bit and encodes the actual bit. Better predictions → fewer output bits.

---

## File Format

```
┌────────────────────────────────────────┐
│ Magic: "KRV\x01" (4 bytes)            │
│ Version: uint8                         │
│ Flags: uint8                           │
│   bit 0: checksum present             │
│   bit 1: original filename stored     │
│   bit 2-7: reserved                   │
│ Original size: uint64 LE              │
│ Block size: uint32 LE                 │
│ Block count: uint32 LE                │
│ [Original filename: null-terminated]  │
├────────────────────────────────────────┤
│ Block 0: [compressed_size: u32] [data] │
│ Block 1: [compressed_size: u32] [data] │
│ ...                                    │
├────────────────────────────────────────┤
│ [CRC-32 of original data: uint32]     │
└────────────────────────────────────────┘
```

---

## Implementation Plan

### Phase 1: Foundation (C core)
1. Arithmetic coder (encode/decode)
2. Basic predictors: Order-0, Order-1, Order-2
3. Simple logistic mixer (fixed model set)
4. Block splitter and file format writer/reader
5. **Milestone:** Compresses and decompresses correctly (round-trip test)

### Phase 2: Full Predictor Suite
6. Add Order-4, Order-8 (hashed contexts)
7. Match finder (sliding window)
8. Run-length and bit-run detectors
9. Sparse context model
10. **Milestone:** Competitive compression ratios on text

### Phase 3: Dynamic Model Spawning
11. Error monitoring and spawn trigger logic
12. Pattern detection (periodicity, record structure)
13. Spawnable model implementations
14. Model retirement and memory management
15. **Milestone:** Measurable improvement on structured binary data

### Phase 4: LZMA-Class LZ Engine
16. Increase LZ window to 4MB (configurable up to 64MB)
17. Replace hash-chain match finder with binary tree (BT4) for optimal matches
18. Implement near-optimal LZ parsing (price-based, not greedy)
19. Position-dependent context modeling for match/literal flags
20. Distance slot encoding with context-modeled bits (like LZMA)
21. Rep-match support (reuse recent match distances cheaply)
22. **Milestone:** Competitive with 7z on binaries and text

### Phase 5: CLI and Integration
16. Python CLI wrapper (`krv compress`, `krv decompress`)
17. Progress bar, error handling, validation
18. CRC-32 integrity checking
19. **Milestone:** Usable end-to-end tool

### Phase 5: Testing and Benchmarking
20. Round-trip correctness on diverse file types:
    - Text (English prose, source code, JSON, XML)
    - Binary (executables, object files)
    - Media (PNG, JPEG, MP3 — already compressed)
    - Structured (CSV, database dumps, log files)
    - Random (incompressible baseline)
21. Compression ratio comparison vs gzip, zstd, xz
22. Speed benchmarks (MB/s)
23. **Milestone:** Published results table

---

## Testing Strategy

| File Type | Source | Sizes |
|-----------|--------|-------|
| English text | Project Gutenberg books | 100KB, 1MB, 10MB |
| Source code | Linux kernel .c files | 100KB, 1MB |
| JSON | Synthetic + real API dumps | 500KB, 5MB |
| CSV | Public datasets | 1MB, 50MB |
| Executable | /usr/bin/* binaries | 1MB, 10MB |
| PNG image | Uncompressed screenshots | 1MB, 5MB |
| JPEG image | Photos | 1MB, 10MB |
| Random | /dev/urandom | 100KB, 1MB |
| Log files | syslog / web server logs | 10MB, 100MB |
| Tar archive | Mixed content tarball | 50MB, 500MB |

**Correctness criterion:** `decompress(compress(file)) == file` byte-for-byte, verified by SHA-256.

**Performance targets (aspirational):**
- Text: within 10% of xz compression ratio
- Binary: within 20% of xz
- Already-compressed: <1% expansion overhead
- Speed: >1 MB/s compress, >5 MB/s decompress (on modern hardware)

---

## CLI Interface

```bash
# Compress
krv compress input.txt                  # → input.txt.krv
krv compress input.txt -o output.krv    # explicit output path
krv compress -b 1M input.bin            # custom block size

# Decompress
krv decompress input.txt.krv            # → input.txt
krv decompress input.txt.krv -o out.txt # explicit output path

# Info
krv info archive.krv                    # show metadata, ratio, block count
```

---

## Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| Model spawning adds overhead without improving ratio | Wasted complexity | A/B test with spawning disabled; make it optional via flag |
| C memory bugs | Crashes, corruption | Valgrind testing, fuzzing with AFL |
| Slow on large files | Unusable for GB scale | Profile early, optimize hot paths, consider SIMD |
| Can't beat zstd on any file type | Disappointing results | Focus on niche wins (structured data, logs) where context mixing excels |
| Arithmetic coder precision issues | Incorrect decompression | Extensive round-trip testing, bit-exact verification |

---

## Dependencies

- **C compiler:** gcc or clang (C11)
- **Python:** 3.10+ (CLI wrapper)
- **Build:** Makefile or CMake
- **Testing:** pytest (Python), custom C test harness
- **No external C libraries** — everything from scratch for the core algorithm

---

## Deliverables

1. `src/` — C source for the compression engine
2. `krv` — Python CLI tool (entry point)
3. `Makefile` — builds the C shared library
4. `tests/` — round-trip correctness tests
5. `benchmarks/` — compression ratio and speed results
6. `README.md` — usage instructions
