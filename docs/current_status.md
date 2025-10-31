# Current Status

**Problem:** Arland DX games have 1.5s lag when opening menus (Rorona/Totori/Meruru DX on Linux/DXVK)

**Root Cause:** ~3000 GPU CopySubresourceRegion calls per menu open, each forcing CPU-GPU sync

**Current State:** Statistics gathered. Found extremely uniform pattern: ~875-900 copies per menu open, ALL matching `512x512 DYNAMIC→STAGING`. Skipping all copies gives 50% improvement but causes glitches. Need to identify which subset of copies is critical.

**Implementation Status:**
- ✅ Implemented texture signature-based statistics tracking
- ✅ Gathered in-game data: 875 copies per menu open, 100% uniform pattern
- ⏳ Next: Implement selective skipping strategies (Options A→B→C)

**Statistics Results:**
- Menu open delta: 2975→3850 copies = **875 copies per menu**
- Pattern: `512x512 DYNAMIC cpu=0x10000 → STAGING cpu=0x20000` (100% of copies)
- Only 1 other pattern seen: `1x1 STAGING→DEFAULT` (irrelevant)

---

## Background

Context: doitsujin's atelier-sync-fix works for Sophie 2 but NOT for Arland DX games.
Goal: Adapt the fix to work with Arland's different rendering pipeline.

## Work Completed

### Phase 1: Diagnosis
- Built minimal D3D11 hook DLL with Map/Unmap/Flush logging
- Captured 30MB+ logs during gameplay with 3 menu opens
- Analyzed log data with Python scripts

### Phase 1 Findings
- 317,471 Map operations total (98.7% WRITE_DISCARD, 1.3% READ)
- 4,010 Flush operations total
- Menu opens show 0 READ operations, all WRITE_DISCARD
- Flush calls ignored successfully (confirmed via logs)
- Flush filtering had no measurable impact on lag (still 1.5s after DLL removed)

### DXVK Metrics (from earlier testing)
During menu opens:
- Queue submissions: 1-5 baseline → 1000 during menu
- Queue syncs: 1-5 baseline → 1000 during menu
- Barriers: 10-50 baseline → 2400 during menu
- CS Syncs: 1-5 baseline → 1000 during menu

## Root Cause Analysis

### Initial Hypothesis (INCORRECT)
- Flush() calls causing GPU sync stalls
- Expected: Filtering 99% of flushes would eliminate lag

### Actual Result
- Flush filtering had zero impact
- Lag remains ~1.5s with or without DLL

### Current Hypothesis
Based on doitsujin's Sophie 2 fix (README.md):
- CopyResource() operations causing CPU-GPU synchronization
- Map() blocking while waiting for CopyResource to complete
- Pattern: CopyResource → Map (blocks) → Unmap → CopySubresourceRegion
- Each cycle forces full CPU-GPU sync
- Multiple sync points per menu open

### Evidence
- Sophie 2 had ~20 GPU sync points in main menu
- doitsujin's fix: shadow buffers to avoid GPU copies
- Arland DX likely has similar pattern but different resource parameters

### Cross-Platform Confirmation
- Issue occurs on Windows native (not DXVK-specific)
- Issue occurs on Switch port
- Pattern suggests engine-level problem from PS4 port (unified memory → discrete GPU)

### Phase 2: Resource Parameter Discovery
- Hooked CopyResource and CopySubresourceRegion
- Logged 3200+ copy operations during menu opens
- Found problematic pattern: ~3000 CopySubresourceRegion calls copying DYNAMIC→STAGING textures

### Phase 2 Findings: Arland vs Sophie 2 Differences

**Arland's Copy Pattern (512x512 Tex2D, Format 90):**
```
DST: STAGING, CPUAccessFlags=0x20000 (READ only), BindFlags=0x0, MiscFlags=0x0
SRC: DYNAMIC, CPUAccessFlags=0x10000 (WRITE only), BindFlags=0x8, MiscFlags=0x0
```

**Why doitsujin's fix fails on Arland:**
- doitsujin's `isCpuWritableResource()` requires `CPUAccessFlags & D3D11_CPU_ACCESS_WRITE`
- Arland's DST staging textures are READ-only (0x20000), not READ+WRITE
- Sophie 2 likely uses READ+WRITE staging textures (0x30000)
- This causes E_INVALIDARG in `tryCpuCopy()` at line 660

**Skipping Test Results:**
- Skipping CopySubresourceRegion: 1.5s → 0.75s lag (50% improvement)
- But causes visual glitches (missing text rendering)
- Skipping CopyResource: No effect on lag, breaks cutscenes

### Phase 3: CPU Shadow Buffer Approach (FAILED)

**What We Tried:**
- Created CPU shadow buffers to mirror DYNAMIC and STAGING GPU textures
- Intercepted Map() to redirect reads/writes to CPU shadow memory
- Intercepted CopySubresourceRegion() to do CPU memcpy instead of GPU copy
- Goal: eliminate GPU-CPU sync by keeping all operations CPU-side

**Results:**
- Successfully intercepted 2000+ CopySubresourceRegion calls
- All calls found valid SRC shadows and performed CPU copies
- **FAILED:** Visual glitches (black regions, missing text, corrupted rendering)

**Why It Failed:**
The game's rendering pipeline includes GPU processing steps that CPU shadows cannot capture:

```
1. CPU Map/Write    → Texture A (various pointers)       [We shadow this ✓]
2. GPU Render       → Texture A → Texture B (0x16b8xxx)  [We can't see this ✗]
3. GPU CopySubregion → Texture B → STAGING C             [We intercept but B has GPU data]
4. CPU Map/Read     → STAGING C                          [We return shadow with wrong data]
```

**Key Evidence:**
- SRC textures in CopySubresourceRegion: `0x16b8f10`, `0x16b8b50`, `0x16b8790`
- These pointers **never appear in Map() calls** - they're purely GPU-generated
- Our CPU shadows copied from wrong textures, missing GPU-rendered content
- Game read corrupted shadow data → visual artifacts

**Conclusion:**
CPU-only shadow buffers cannot work when GPU rendering creates intermediate textures between Map operations. Would need GPU pipeline hooks (impossible) or different approach.

## Selective Skipping Strategies

**Problem:** Pointer-based tracking unreliable due to DXVK object pooling/reuse. Need alternative methods to identify which copies are critical vs wasteful.

### Option A: Percentage-Based Skipping (Simplest)

**Approach:**
- Skip N% of copies (e.g., 90%, 95%, 99%)
- Let through every 10th/20th/100th copy
- Use simple counter: `if (++counter % 10 != 0) skip;`

**Pros:**
- Dead simple implementation
- Zero tracking overhead
- Immediate testing

**Cons:**
- Blind guessing - might skip critical copies
- No learning/adaptation

### Option B: Map(READ) Rate Tracking (Diagnostic)

**Approach:**
- Count total CopySubresourceRegion calls
- Count total Map(READ) calls on STAGING textures
- Calculate ratio: `read_count / copy_count`
- If ratio is low (e.g., 5%), most copies are wasted

**Pros:**
- Tells us if skipping is viable at all
- No pointer tracking needed (just counts)
- Informs how aggressive Option A can be

**Cons:**
- Doesn't tell us WHICH copies to skip
- Purely diagnostic, not a fix

### Option C: Texture Content Checksumming (Most Robust)

**Approach:**
- When game Maps DYNAMIC texture for WRITE: checksum pixel data before Unmap
- When game Maps STAGING texture for READ: checksum pixel data, mark checksum as "used"
- Build allowlist of checksums that are actually read
- Skip CopySubresourceRegion for textures whose checksums never appear in reads

**Pros:**
- Identifies specific texture content that's needed
- Works despite pointer reuse (content-based, not pointer-based)
- Can build profile over multiple gameplay sessions

**Cons:**
- CPU cost of checksumming ~1MB textures (but still < 1500ms GPU sync cost)
- Complex implementation
- Requires extra Map(WRITE) calls on STAGING to checksum after copy

**Implementation Plan:**
1. Try Option A first (quick test)
2. Implement Option B to validate hypothesis
3. If B shows low ratio, implement Option C for surgical skipping



## References
- doitsujin atelier-sync-fix: https://github.com/doitsujin/atelier-sync-fix
- Sophie 2 had CopyResource → Map(READ_WRITE) pattern causing sync
- Arland has DYNAMIC WRITE → STAGING READ pattern
- DXVK private data gotcha: wrapper objects don't preserve SetPrivateDataInterface across calls
