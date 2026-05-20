# KRV Novel Algorithm Ideas

## 1. Predictive Match Valuation (PMV)

Instead of optimal parsing (backward bit-cost DP), look forward at how a match affects future compressibility. After finding candidate matches at position P, simulate the CM state after each match ends. Pick the match that minimizes `encode_cost(match) + predicted_cost(next_N_bytes_given_context)`.

Uses the live mixer's actual predictions as the cost model rather than a static price function. 7z can't do this — LZMA's price model is a fixed approximation. We have a live context mixer we can query directly.

**Impact:** 5-15%  
**Effort:** High (forward simulation per candidate)  
**Novelty:** Very high — no known compressor does this

---

## 2. Structural Resonance Detection (SRD) / Stride Predictor

Detect the dominant period/stride in a block via autocorrelation and add a stride predictor to the mixer that predicts `current_byte = byte[pos - stride]`.

- Autocorrelate first 4KB of each block
- Find dominant period (52 bytes for fixed-width CSV, 4 for 32-bit aligned, ~80 for log lines)
- Mixer learns the weight — if stride is real, this predictor dominates

More general than delta/BCJ because it discovers structure rather than assuming it. Logs with 80-byte lines would have a predictor that's right 70%+ of the time on repeated fields.

**Impact:** 10-20% on logs/structured data  
**Effort:** Low (one new predictor + autocorrelation)  
**Novelty:** High — no mainstream compressor auto-detects stride

---

## 3. Entropy-Gated Match Selection

Byte-level hybrid instead of block-level. Run the CM during LZ encoding and at each position:

- If CM entropy is very low (< 1 bit/byte for next N bytes), skip the match — CM encodes it cheaper
- If CM entropy is high (> 6 bits/byte), prefer longer matches even at worse distances

Creates dynamic switching between "let the mixer handle this" and "use LZ here" within a single block. No other compressor does this — they commit to one method per block/stream.

**Impact:** 5-10%  
**Effort:** Medium (CM must run alongside LZ encoder)  
**Novelty:** High

---

## 4. Cascading Context Inheritance

When a match is copied, the CM state goes stale. Novel approach:

- During match copy, run CM forward through matched bytes in background (sync)
- Create a **match-exit context**: "just finished match of length L from distance D, last 4 bytes were X"
- Use this rich context to predict first few bytes after the match with higher accuracy

The bytes immediately after a match are highly constrained (they're whatever didn't match). A specialized post-match predictor could nail these transitions.

**Impact:** 2-5%  
**Effort:** Low-Medium  
**Novelty:** High

---

## 5. Adaptive Alphabet Remapping (Per-Context MTF)

Instead of encoding raw bytes, maintain a move-to-front list per order-2 context. Encode the MTF rank instead of the raw byte. Ranks cluster near 0, making the arithmetic coder's job easier.

Similar to BWT+MTF but applied locally per context without global block sort. The mixer sees rank values with inherently lower entropy.

**Impact:** 2-4%  
**Effort:** Low  
**Novelty:** Medium (MTF is known, but per-context application in a CM is not standard)

---

## 6. Cross-Predictor Correlation Exploitation

Detect when two predictors are correlated (both wrong together) and add a correction predictor modeling their joint error.

- Track `error_i XOR error_j` for each predictor pair
- When correlation is high, spawn a correction model
- Feed into mixer as additional signal

Inspired by boosting — each new model focuses on errors of the previous ensemble.

**Impact:** 2-4%  
**Effort:** Medium  
**Novelty:** High

---

## 7. Speculative Parallel Contexts (Specialist Predictors)

Run multiple specialist models in parallel with different data-type assumptions, let the mixer arbitrate:

- **x86 specialist**: predicts E8/E9 patterns, instruction-length-aware contexts
- **UTF-8 specialist**: predicts continuation bytes, common word bigrams
- **Record specialist**: uses detected stride for prediction
- **Noise specialist**: always predicts 0.5 (signals incompressible regions)

Replaces hardcoded filters with learned soft-switching. The x86 specialist doesn't need to be a BCJ filter — it just needs to predict well on code, and the mixer upweights it automatically.

**Impact:** 5-10% on binaries/text  
**Effort:** Medium (new predictor types)  
**Novelty:** Medium-High

---

## Priority Ranking

| # | Idea | Impact | Effort | Best For |
|---|------|--------|--------|----------|
| 1 | Stride Predictor (SRD) | High | Low | Logs, structured data |
| 2 | Entropy-Gated Match Selection | Medium-High | Medium | All files |
| 3 | Speculative Parallel Contexts | High | Medium | Binaries, text |
| 4 | Predictive Match Valuation | Very High | High | All files |
| 5 | Cascading Context Inheritance | Medium | Low-Medium | LZ-heavy files |
| 6 | Adaptive Alphabet Remapping | Medium | Low | Text, source |
| 7 | Cross-Predictor Correlation | Medium | Medium | All files |

---

## 7. Speculative Future-Cost Match Selection

Instead of a static price model, clone the CM state at each DP position and simulate forward 8-16 bytes after each candidate match ends. Pick the match that leaves the CM in the best predictive state for what follows.

The insight: two matches of the same length/distance can have wildly different value depending on what context they leave behind. A match ending at a word boundary is worth more than one ending mid-word. No compressor does this — they all use backward-looking cost models.

**Impact:** 5-15%  
**Effort:** High (CM state cloning + forward simulation per candidate)  
**Novelty:** Very high

---

## 8. Inverse Match Prediction (Two-Pass Future-Aware Encoding)

Two-pass compression where pass 1 identifies all match targets (which byte sequences will be referenced later), and pass 2 encodes with that knowledge. Bytes that are match sources ("seeds") get encoded more carefully since errors propagate. Bytes never referenced can be matched more aggressively.

More radically: encode "seed" hints so the decoder knows upcoming data will be reused, enabling it to allocate better prediction resources to those regions.

**Impact:** 3-8%  
**Effort:** High (two full passes, bookkeeping)  
**Novelty:** Very high — no known compressor exploits future reference knowledge

---

## 9. Differential Context Mixing

Instead of predicting absolute bit probability, predict the *change* from the previous bit's probability. In structured data, consecutive bits within a byte are correlated — if bit 6 was a surprise, bit 5 likely is too. Train a "delta mixer" on `P(bit_n) - P(bit_{n-1})` and use it to correct the main mixer.

**Impact:** 1-3%  
**Effort:** Low-Medium  
**Novelty:** High — no known CM uses inter-bit probability differentials

---

## 10. Match Topology Optimization (Rep-Slot Lookahead)

Scan ahead and count how many times each distance appears in the next N positions. If distance D appears 20 times in the next 4096 bytes, it's worth paying extra now to get D into the rep slots early — even if the current match at distance D is slightly worse than an alternative.

This is deliberate "rep-slot planting" — sacrificing local optimality to set up future cheap rep-matches. The DP tracks rep state but doesn't look ahead at future demand for specific distances.

**Impact:** 3-8% on binaries (where rep patterns are structural)  
**Effort:** Medium (distance histogram per DP window + modified pricing)  
**Novelty:** Very high — no compressor optimizes rep-slot contents proactively

---

## 11. Entropy-Gated Per-Position Literal Pricing

During pass 1 (BT match collection), also run the CM forward and record its prediction entropy at each position. Feed this as a per-position literal cost into the DP instead of the global constant.

Positions where CM predicts well (low entropy) get cheap literal prices → DP prefers literals there. Positions where CM struggles (high entropy) get expensive literal prices → DP prefers matches there. This creates automatic byte-level LZ/CM switching within the DP framework.

**Impact:** 5-10% (directly addresses the fixed-literal-price weakness)  
**Effort:** Medium (CM already runs during sync; need to record per-position costs)  
**Novelty:** High — no known compressor uses live entropy feedback in optimal parsing
