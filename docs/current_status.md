# Current Status

**Problem:** Arland DX games have 1.5s lag when opening menus (Rorona/Totori/Meruru DX on Linux/DXVK)

**Root Cause:** ~931 GPU CopySubresourceRegion calls per menu open, each forcing CPU-GPU sync. Each of 15 numeric characters rendered ~31 times (massive redundancy).

**Current State:** Statistics gathered. Found ~931 copies per menu open, ALL matching the Arland pattern: `512x512 DYNAMIC→STAGING at position (0,0,0)`. Skipping all copies gives 50% improvement but causes missing text (15 numeric characters break).

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

## Phase 4: Map(READ) Tracking Results

**Latest instrumentation with Map(READ) counting:**

Menu open delta:
- STAGING copies: 2783 - 1016 = **1767 copies**
- STAGING reads: 2782 - 1015 = **1767 reads**
- Waste ratio: **~0%**

**Key Finding:** THERE ARE NO WASTEFUL COPIES. Every copy is immediately followed by a Map(READ).

**Actual Copy Pattern (from logs):**
```
Copy: 512x512 DYNAMIC → 200x200 STAGING (partial region copy)
Map(READ): 512x512 STAGING texture
```

Pattern repeats 1767 times during menu open. This is **texture atlas packing** - copying 200x200 subregions from rendered glyphs.

**Why Is It Slow?**

The 1.5s lag comes from CPU-GPU synchronization on every copy:

```
For each of 1767 textures:
1. GPU renders glyph to 512x512 DYNAMIC texture
2. CopySubresourceRegion(DYNAMIC → STAGING, 200x200 box)  ← Forces GPU flush
3. Map(READ) on STAGING                                    ← CPU waits for GPU
4. CPU reads pixel data (likely for atlas building)
5. Unmap
```

Total: 1767 × ~0.85ms per sync = ~1.5s

**Breakdown of 1.5s lag:**
- ~0.75s: GPU rendering 1767 glyphs
- ~0.75s: CPU-GPU sync overhead from Map(READ) waiting

**Why Phase 2's "Skip CopySubresourceRegion" gave 50% speedup:**
- Skipping copy → Map(READ) reads uninitialized data instantly (no GPU wait)
- Eliminates the sync overhead (~0.75s saved)
- But GPU rendering still happens (~0.75s remains)
- Text disappears because CPU gets garbage data for atlas building

## Phase 5: Map(READ) Caching Test (COMPLETED)

**Implementation:** Cache first Map(READ) result per texture pointer, return cached data on subsequent reads.

**Results:**
- Menu lag: 1.5s → 0.75s (50% improvement)
- Text rendering: Corrupted (wrong letters, garbled layout)
- Happens immediately on first menu open, not just when values change

**Why it failed:**
- DXVK reuses texture pointers (object pooling)
- Same pointer used for glyph 'A', then 'B', then 'C'
- Cache returns 'A' pixels for all subsequent glyphs
- Game builds corrupted text from wrong glyph data

**Comparison with Phase 2 (skipping CopySubresourceRegion):**
- Skipping copies: STAGING has uninitialized memory → no text displays at all
- Cached reads: STAGING has wrong glyph → text displays but corrupted
- Both fail immediately, not dependent on value changes

## Phase 6: Content-Based Caching Investigation

**Theory:** Cache readback data by content (first N bytes as key) to avoid GPU syncs on duplicate glyphs.

**Why This Doesn't Work - Chicken and Egg Problem:**

To use content-based caching:
1. Need to identify which glyph before doing GPU sync
2. To identify glyph → need to read pixel data
3. To read pixel data → need Map(READ)
4. Map(READ) → **GPU sync** (the thing we're trying to avoid!)

**The fundamental issue:** We can't know the content without reading it, and we can't read it without syncing.

**Alternative approaches considered:**
- Hash first N bytes: Still requires Map(READ) to extract bytes for hashing
- Track copy source pointers: DXVK reuses pointers (unreliable)
- Cache by texture pointer: DXVK object pooling breaks this (proven in Phase 5)
- Cache by frame number: No correlation to menu text content

**Key insight:** Any caching solution needs a stable texture ID/handle, and pointer-based approaches are unreliable in DXVK.

**Conclusion:** Content-based caching cannot avoid the initial GPU sync. Best case: first menu open slow, subsequent faster (but user's stats change every menu, so limited benefit).

## Phase 7: The (0,0,0) Discovery (COMPLETED)

**Discovery:** When skipping CopySubresourceRegion, only ~50 characters break on main menu:
- "Population" value
- "Development points" value
- "Next rank" value
- Rest of menu text remains intact (rendered via different pipeline - GPU-only, no readbacks)

**Investigation:** Added destination position tracking to understand the copy pattern.

**Results:**
```
DESTINATION POSITIONS:
  Unique destination positions: 1
  Top destination positions:
    (0,0,0): 2998 copies
```

**ALL copies go to position (0,0,0)!**

## What This Means

The game is **NOT** building a texture atlas. It's using the STAGING texture as a **temporary scratchpad**.

**The actual pattern:**
```
For each of ~931 glyphs per menu open:
1. GPU renders glyph to 512x512 DYNAMIC texture
2. CopySubresourceRegion(DYNAMIC → STAGING at position 0,0,0)  [overwrites previous]
3. Map(READ) on STAGING texture
4. CPU processes the glyph pixels (likely for layout/metrics)
5. Unmap
6. Repeat with next glyph (position 0,0,0 gets overwritten)
```

**Why only 15 numeric characters break when skipping copies:**
- **Confirmed via testing:** On main menu, only the **numeric values** fail to render when copies are skipped:
  - "Population" value (e.g., "123456" = 6 digits)
  - "Development points" value (e.g., "9999" = 4 digits)
  - "Next rank" value (e.g., "50000" = 5 digits)
  - **Total: ~15 numeric characters**
- All other menu text (titles, labels, button text, even the label portions like "Population:") renders correctly without readbacks
- Most menu text uses GPU-only rendering pipeline (no CPU readback needed)
- Only these dynamic numeric values use the readback pipeline

**Important note:** The stat values don't change frequently during normal gameplay. When testing with sequential menu opens where values remain constant, the same glyphs are re-rendered and re-read every time (no caching in game engine).

**Key insight:** The copies are sequential and non-persistent. Each glyph overwrites the previous one at (0,0,0). The game doesn't need an atlas - it processes glyphs one-by-one and discards the readback data after processing.

**Why this is slow:**
Each of the ~931 readbacks forces a GPU sync. The game could batch these or cache results, but instead does them sequentially with full CPU-GPU synchronization each time.

**Critical Constraint:**
We **cannot** distinguish "menu open copies" from "cutscene copies" from "startup copies" at the D3D11 API level. All we can detect is the Arland pattern itself. Any solution must work globally across all game contexts.

**Why Can't We Use Smart Caching?**
- **Content-hash caching**: Requires Map(READ) to get content → triggers the expensive GPU sync we're trying to avoid (chicken-and-egg problem)
- **Pointer-based caching**: DXVK/game reuses same pointers for different glyphs (proven in Phase 5)
- **Sequence-based tracking**: No way to know when a "burst" starts/ends or what game context we're in

**Key Observation:**
Skipping **all** Arland pattern copies doesn't crash the game - it only causes missing/corrupted text. This proves the copies aren't structurally necessary, only the readback data matters for text rendering.

**Remaining Questions:**
1. **Why redundancy per character?** Possible explanations:
   - Multiple font sizes/styles rendered but only one used?
   - Antialiasing passes
   - Game engine bug (rendering same glyph unnecessarily)?

2. **Are the copies identical or different?**
   - Same source texture pointer repeated? (suggests identical glyph)
   - Different source pointers? (suggests different rendering passes)
   - Same box dimensions? (suggests same glyph region being copied)

3. **Timing pattern:**
   - Do copies happen in rapid burst (<100ms)?
   - Or spread across the entire menu session?
   - Are there natural gaps we can exploit?

**Next Investigative Steps:**
1. Track source texture pointer patterns
2. Track box dimensions across consecutive copies (are they identical?)
3. Track timing between copies (tight bursts vs spread out?)
4. Count unique src/dst pointers used during a single menu open

**Option: Texture Lifecycle Tracking**
- Hook CreateTexture2D and Release to track texture allocation/deallocation
- Assign stable IDs to textures independent of pointer values
- Enables "copy count per texture ID" tracking that survives pointer reuse
- More complex, requires hooking ID3D11Device interface


## References
- doitsujin atelier-sync-fix: https://github.com/doitsujin/atelier-sync-fix
- Sophie 2 had CopyResource → Map(READ_WRITE) pattern causing sync
- Arland has DYNAMIC WRITE → STAGING READ pattern
- DXVK private data gotcha: wrapper objects don't preserve SetPrivateDataInterface across calls
