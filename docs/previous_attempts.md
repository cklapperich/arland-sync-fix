
## Previous Fix Attempts

### Phase 1-4: Flush Filtering
- **Result:** No effect (Flush wasn't the problem)

### Phase 5: Pointer-Based Caching
- **Result:** Corrupted text (DXVK reuses pointers for different glyphs)

### Phase 6: Content-Based Caching
- **Result:** Chicken-and-egg (need to Map(READ) to get content → triggers sync)

### Phase 7: Understanding (0,0,0)
- **Discovery:** All copies go to position (0,0,0) in STAGING
- **Insight:** Not building atlas, just sequential scratchpad processing

### Phase 8: CPU Shadow Buffers
- **Result:** Visual glitches (GPU renders to textures we can't shadow)

### Phase 9: Write/Read Correlation Analysis
- **Logged:** Map(WRITE_DISCARD) + CopySubresourceRegion + Map(READ) full pipeline
- **Discovery:** 11.7% of writes never result in readbacks (redundant)
- **Pattern identified:** Redundant writes occur in predictable bursts at start of rendering sequences
- **Burst pattern:** 8+ consecutive writes with no Copy/Read between them, only last write gets read
- **Normal pattern:** 1:1 Write→Copy→Read (immediate readback after each write)
- **Why this doesn't help:** CPU writes are cheap (~negligible time). The 1.5s bottleneck is GPU sync during Map(READ), not the writes themselves. Skipping burst writes would save <10ms out of 1500ms.