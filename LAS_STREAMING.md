# KRV-LAS: Streamable Point Cloud Compression

## Concept

Use KRV as a LAS compressor with spatial blocking, enabling random-access streaming that LAZ cannot do.

## Why KRV Fits

- LAS has fixed-stride records (20-34 bytes/point) → stride transpose auto-triggers
- Spatially sorted X/Y/Z have tiny deltas → CM + stride predictor excels
- Classification/intensity are locally repetitive → CM handles well
- Expected to beat LAZ on ratio (cross-field correlation + better entropy coding)

## Architecture

- Block boundaries = spatial tiles (Morton/Hilbert curve ordering)
- Block index at file start: (bbox, compressed_offset, n_points) per tile
- Each block is independently decompressible
- Spatial query → look up intersecting tiles → decompress only those blocks

## Advantages Over LAZ

- Spatial random access (LAZ chunks are sequential, not spatial)
- True streaming: pan/zoom fetches only needed tiles
- Better compression from cross-field CM correlation
- Progressive loading (render first tile immediately)

## Implementation Estimate

- LAS header parser: ~50 lines
- Spatial sort + tiling: ~100 lines
- Block index format: ~30 lines
- Reader API (query_bbox): ~50 lines
- Compression already works via stride detection

## Status

Idea only. Implement after speed optimization is complete.
