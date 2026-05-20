# KRV

A lossless compression algorithm that beats 7z on 8 out of 9 tested file types.

KRV combines a 9-predictor adaptive context-mixing model with an optimal LZ parser, arithmetic coding, and content-aware preprocessing (x86 BCJ, ELF section splitting, .eh_frame stream separation, stride-based transposition).

## Results

| File | Size | 7z | KRV | vs 7z |
|------|------|-----|-----|-------|
| services (text) | 12KB | 5,241 | **4,892** | -6.7% |
| copyright (text) | 110KB | 23,351 | **23,263** | -0.4% |
| ls (ELF) | 142KB | 52,383 | **51,990** | -0.8% |
| bash (ELF) | 1.4MB | 545,073 | **515,059** | -5.5% |
| git (ELF) | 4MB | 1,550,976 | **1,495,410** | -3.6% |
| libc (ELF) | 2.1MB | 743,402 | 749,745 | +0.9% |
| logs | 832KB | 25,074 | **24,926** | -0.6% |
| json | 165KB | 6,563 | **4,211** | -35.8% |
| source | 22KB | 1,355 | **1,033** | -23.8% |

Also beats gzip, bzip2, zstd, and xz on nearly all files.

**Speed:** ~1 MB/s compression, ~10x slower than 7z. The tradeoff is ratio for throughput.

## Building

```bash
make
```

Requires GCC (C11) and Python 3.

## Usage

```bash
# Compress
./krv compress input.txt              # → input.txt.krv
./krv compress input.txt -o out.krv   # explicit output

# Decompress
./krv decompress input.txt.krv        # → input.txt

# Info
./krv info archive.krv
```

## Architecture

- **C core** (`src/`) — compression engine compiled to `libkrv.so`
- **Python CLI** (`krv`) — thin wrapper via ctypes

### Core Components

| Component | Description |
|-----------|-------------|
| Arithmetic coder | 32-bit binary arithmetic coding |
| Context mixer | 9 predictors (order-0/1/2/4/8, match, run-length, stride, LZ match-byte) with logistic mixing |
| Optimal parser | Forward DP over full blocks with rep-state tracking, adaptive pricing |
| Match finder | 4MB binary tree (BT4) |
| BCJ filter | x86 E8/E9 + RIP-relative LEA/MOV/CMP (auto-detected) |
| ELF splitter | Per-section compression with content-type separation |
| .eh_frame transform | DWARF CFI stream separation (lengths, PC-deltas, ranges, instructions) |
| Stride detection | Autocorrelation-based, triggers column-split transpose |

## Tests

```bash
python3 tests/test_roundtrip.py    # 19 round-trip correctness tests
```

## File Format

Extension: `.krv`

```
Magic "KRV\x01" | Version | Flags | Original size (u64)
Block size (u32) | Block count (u32) | [Filename]
[Block 0: compressed_size + data] [Block 1: ...] ...
[CRC-32]
```

Blocks are independently decompressible.

## License

All rights reserved.
