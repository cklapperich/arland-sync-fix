# Arland DX Menu Lag

-1.5s lag when opening main menus in Arland DX games (Rorona/Totori/Meruru DX on Linux/DXVK)

- 1,018 synchronous GPU→CPU readbacks per menu open

- 528 in blocked in Map(READ) calls + additional GPU work

- doitsujin's atelier-sync-fix works for Sophie 2 but NOT for Arland DX games due to different rendering pipeline and resource flags.

## Metrics from 1 main menu open in Atelier: Meruru DX

```
Total operations: 1,018 Copy→Map(READ)→Unmap sequences
Total time: 1,431 ms (1.43 seconds)
Time blocked in Map(READ): 528 ms (36.9% of total)
Average block per Map(READ): 0.52 ms

Source textures: 3 DYNAMIC textures (triple-buffered)
  - 0x2ea7880: 328 copies (32.2%)
  - 0x2ea7c40: 348 copies (34.2%)
  - 0x2ea8000: 342 copies (33.6%)

Destination textures: 64 STAGING textures
  - Most used: 0x3090130 (83 copies)
  - 51/64 destinations receive from all 3 sources
```

### Visual Representation
```
Sequence: CBACBACACBACBAACBACBACCBACBACBACBACBACBACBACBACBAC...
Pattern: A→C→B→A→C→B→A→C→B (near-perfect triple-buffering)
Match rate: ~43% perfect cycle (some deviations)
```

### What are the map(read) targetting?

Believed the problem is down to ~15 numeric characters on the main menu:
- "Population" value (e.g., "123456")
- "Development points" value (e.g., "9999")
- "Next rank" value (e.g., "50000")

### What's Happening
1. Game renders glyph to DYNAMIC texture (GPU)
2. CopySubresourceRegion: DYNAMIC → STAGING (GPU operation)
3. **Map(READ) on STAGING - BLOCKS HERE waiting for GPU**
4. CPU reads glyph pixel data
5. Unmap
6. Repeat 1,018 times

All other menu text renders GPU-only (no readback). I think that only these dynamic numeric values use the readback pipeline? (confirm?)
'proof' is that  if you skip the copyresourceregion entirely with a dx3d11 patch, the 'dynamic' text goes blank.

### The CPU-GPU Pipeline

  1. Map(WRITE_DISCARD) on DYNAMIC texture 0x2d445f0
  2. Unmap (CPU writes data during this Map/Unmap pair)
  3. CopySubresourceRegion: DYNAMIC 0x2d445f0 → STAGING 0x2ba4bc0  (GPU copies it) - causes lag
  4. Map(READ) on STAGING 0x2ba4bc0 (CPU reads it back - THIS CAUSES LAG)
  5. Unmap

For each of ~1,018 glyphs:
┌─────────────────────────────────┐
│ CPU: Map(WRITE_DISCARD)         │ Maps DYNAMIC texture for writing
│      on DYNAMIC texture          │
└────────────┬────────────────────┘
             ↓
┌─────────────────────────────────┐
│ CPU: Write glyph parameters     │ Character, font, size, position?
└────────────┬────────────────────┘
             ↓
┌─────────────────────────────────┐
│ CPU: Unmap                       │
└────────────┬────────────────────┘
             ↓
┌─────────────────────────────────┐
│ GPU: Render glyph using params  │ (different for each character)
└────────────┬────────────────────┘
             ↓
┌─────────────────────────────────┐
│ GPU: CopySubresourceRegion      │ (512x512 DYNAMIC → STAGING)
└────────────┬────────────────────┘
             ↓
┌─────────────────────────────────┐
│ CPU: Map(READ) on STAGING       │ ← BLOCKS 0.52ms avg waiting for GPU
└────────────┬────────────────────┘
             ↓
┌─────────────────────────────────┐
│ CPU: Read rendered glyph pixels │ (layout/metrics calculation?)
└────────────┬────────────────────┘
             ↓
┌─────────────────────────────────┐
│ CPU: Unmap                       │
└─────────────────────────────────┘
```

**Total: 1,018 iterations × ~1.4ms each = 1.43 seconds**

**Key Insight from Phase 9 (Detailed Write Logging):**

With Map(WRITE_DISCARD) logging enabled:
- **2,036 Map(WRITE_DISCARD) operations** per menu open (DXVK logging found 317,471 total - mostly unrelated?)
- **1,018 CopySubresourceRegion** operations (matches previous findings)
- **1,018 Map(READ)** operations

- **238 out of 2,036 writes (11.7%) are NEVER followed by readback**
- These redundant writes occur across all 3 DYNAMIC textures:
  - 0x2d445f0: 90.4% readback rate (65 redundant writes)
  - 0x2d449b0: 84.5% readback rate (105 redundant writes)
  - 0x2d44d70: 90.0% readback rate (68 redundant writes)

**Pattern Discovery:**
The 11.7% no-readback writes occur in **predictable bursts**:

```
Burst pattern: W W W W W W W W → only last write gets Copy→Read
Normal pattern: W → Copy → Read (immediate, 100% readback)
```

**Example from trace:**
- Lines 1-8: 8 consecutive WRITES (A,B,C,A,B,C,A,B) with NO Copy/Read
- Line 9: COPY+READ of only the LAST write (B)
- Lines 11+: Normal 1:1 Write→Copy→Read pattern

Writes in multi-write bursts (no Copy/Read between them) are redundant except the last one.
These bursts are scattered throughout (likely pre-rendering/setup phases).

**Why Write Bursts Don't Help Performance:**
- Bursts are detectable in real-time (consecutive writes without Copy/Read)
- Could skip dummy writes in bursts, but writes aren't the bottleneck
- The expensive operation is Map(READ) blocking on GPU sync (0.5ms each × 1,018 = 500ms+)
- CPU writes to mapped memory are nearly free

**Additional Context:**
- Issue affects other areas of UI UI (certain other menus and tem screens that render text glyphs or even non-text 512x512 glyphs), not just main menu
- Values within a single menu open are static, but change between opens
- Same texture objects are reused for different glyphs within a single frame
- No way to detect content changes without reading (which requires GPU sync)

## Why doitsujin's Fix Doesn't Work

**Sophie 2 pattern:**
```
SRC: CPU-writable (CPUAccessFlags = 0x30000 - READ+WRITE)
DST: CPU-writable (CPUAccessFlags = 0x30000 - READ+WRITE)
Fix: CPU memcpy instead of GPU copy
```

**Arland DX pattern:**
```
All copies: 512x512 Tex2D, Format 90, position (0,0,0), box=full
SRC: DYNAMIC WRITE-only (CPUAccessFlags = 0x10000, BindFlags = 0x8)
DST: STAGING READ-only (CPUAccessFlags = 0x20000, BindFlags = 0x0)

Problems:
1. doitsujin's isCpuWritableResource() requires D3D11_CPU_ACCESS_WRITE
   - Arland's DST is READ-only (0x20000), not READ+WRITE (0x30000)
   - Causes E_INVALIDARG in tryCpuCopy() at line 660

2. GPU rendering creates intermediate textures not visible to CPU hooks
   - Source textures in CopySubresourceRegion never appear in Map() calls
   - CPU shadows copy from wrong textures, missing GPU-rendered content
   - Results in visual glitches (black regions, missing text)
```

## Timing Gaps Analysis

```
Gaps between copies:
  p50: 1.000 ms
  p90: 1.000 ms
  p99: 8.000 ms

Large gaps (>5ms): 15 instances
  Largest: 112ms gap (after copy #367)

Interpretation: Operations are mostly sequential with occasional
               frame boundaries or other game processing
```

### ⚠️ Option 4: Stable Texture IDs
- Hook CreateTexture2D/Release
- Assign stable IDs independent of pointers
- Track texture lifetime and content
- **Problem:** Complex, requires hooking ID3D11Device interface
- **Benefit:** Would enable proper caching across pointer reuse

**DXVK Metrics During Menu Opens:**
- Queue submissions: 1-5 baseline → 1000 during menu
- Queue syncs: 1-5 baseline → 1000 during menu
- Barriers: 10-50 baseline → 2400 during menu
- CS Syncs: 1-5 baseline → 1000 during menu

**Important Notes:**
- DXVK wrapper objects don't preserve SetPrivateDataInterface across calls? (verify?) (can't use for tracking?)
- All copies overwrite position (0,0,0) in STAGING - not building atlas, just sequential processing
- Only 15 numeric characters need readbacks, but game does 1,018 operations (redundancy unexplained)

## References

- doitsujin atelier-sync-fix: https://github.com/doitsujin/atelier-sync-fix
- Sophie 2 had CopyResource → Map(READ_WRITE) pattern causing sync
- Arland has DYNAMIC WRITE → STAGING READ pattern with different flags
