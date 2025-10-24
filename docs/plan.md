# Atelier Arland DX Menu Lag Fix Project

## Project Goal

Eliminate the 2+ second menu lag when opening the cauldron, container, or main menu in Atelier Meruru DX (and potentially Rorona/Totori DX) on PC, particularly when running through Proton/Linux.

**Target:** Create a custom DirectX 11 hook/fix that addresses GPU synchronization stalls specific to the Arland DX engine.

---

## Problem Statement

### Symptoms
- **Consistent 2-second freeze** when opening any menu (cauldron, container, main menu)
- FPS counter completely stops updating during freeze
- Brief drop to ~8fps for 1 frame when menu finally opens
- Returns to normal 120fps immediately after
- **Lag is consistent** - does not improve over time or with repeated menu opens
- Issue affects all three Arland DX games (Rorona/Totori/Meruru)

### Root Cause (Confirmed Pattern Across All Atelier PC Ports)

**GPU synchronization stalls caused by naive PS4 → PC porting.**

All Atelier PC games are PS3/PS4 ports and suffer from this issue to varying degrees. The problem stems from a fundamental architectural mismatch:

**PS4 (Unified Memory):**
- CPU and GPU share the same 8GB GDDR5 memory
- Writing data for GPU use requires no copying or synchronization
- Game code: "write data, GPU uses it" in one operation

**PC (Discrete GPU):**
- CPU (System RAM) and GPU (VRAM) have separate memory spaces
- Data must be copied across PCIe bus (slow)
- Requires explicit synchronization and staging buffers

**What Gust's Engine Does (Naive Port):**

The engine was written for PS4's unified memory and never adapted for PC's discrete GPU architecture. When loading UI elements:

```cpp
// PS4: Fast (unified memory)
for (int i = 0; i < 1000; i++) {
    LoadUIElement(i);  // Just write to shared memory
}
// GPU can immediately use the data

// PC Port: Slow (discrete GPU, but same logic!)
for (int i = 0; i < 1000; i++) {
    context->Map(uiTexture[i], D3D11_MAP_WRITE_DISCARD, ...);
    memcpy(mapped.pData, uiData[i], size);
    context->Unmap(uiTexture[i]);
    context->Flush();  // ❌ Force synchronous copy across PCIe!
    // Wait for GPU to be ready for next element
}
// 1000 individual PCIe round-trips = 2+ second freeze
```

**This pattern appears in every Atelier PC port** - Gust makes the same mistake repeatedly. The newer games (Sophie 2, Ryza 3) got fixed by doitsujin, but his fix doesn't work for Arland's older engine version.

### Platform Details
- **Primary Testing:** Atelier Meruru ~The Apprentice of Arland~ DX
- **OS:** Pop!_OS Linux (Ubuntu-based)
- **Compatibility Layer:** Steam Proton (Wine + DXVK)
- **DXVK:** DirectX 9/10/11 → Vulkan translation layer
- **Game Engine:** DirectX 11 (confirmed via system requirements)
- **Original Platform:** PS4 port of PS3 remake

---

## Technical Background

### The GPU Sync Problem

From doitsujin's documentation on Sophie 2 (newer Atelier games):

```cpp
// What the game does (pseudo-code):
ID3D11Buffer* stagingBuffer;
d3d11Device->CreateBuffer(&desc, nullptr, &stagingBuffer);
d3d11Context->CopyResource(stagingBuffer, gpuResource);  // GPU operation
d3d11Context->Map(stagingBuffer, 0, D3D11_MAP_READ_WRITE, 0, &mapped);  // BLOCKS until GPU finishes
// ... CPU work with mapped buffer
d3d11Context->Unmap(stagingBuffer, 0);
d3d11Context->CopySubresourceRegion(gpuResource, 0, ..., stagingBuffer, 0, ...);
```

**The problem:** `Map()` blocks the CPU until all previous GPU operations complete. When this happens multiple times per frame (Sophie 2 has ~20 sync points in menus), it creates massive stalls.

Sophie 2 shows this as underutilization (60fps → 144fps with fix). Arland DX shows this as complete freezes (2+ seconds).

### DirectX 11 API Chain

On Linux through Proton:
```
Game (Windows .exe)
    ↓
Wine (Windows API emulation)
    ↓
DXVK (DirectX 11 → Vulkan translator)
    ↓
Vulkan drivers
    ↓
GPU
```

Any fix must work at one of these layers:
1. **Game level:** Inject DLL that hooks D3D11 calls (doitsujin's approach)
2. **DXVK level:** Modify DXVK to optimize these patterns
3. **Hybrid:** Detect patterns in DXVK and optimize automatically

---

## Existing Fixes & Why They Don't Work

### 1. doitsujin's atelier-sync-fix

**Project:** https://github.com/doitsujin/atelier-sync-fix  
**Author:** Philip Rebohle (creator of DXVK)  
**Target Games:** Sophie 2, Ryza 3, and other newer Atelier games  
**Release:** https://github.com/doitsujin/atelier-sync-fix/releases

#### How It Works
- Hooks D3D11 API calls via wrapper DLL (`d3d11.dll`)
- Creates shadow staging buffers on CPU for GPU resources
- Updates shadows when resources change
- When game calls `CopyResource()`, copies from shadow instead (instant CPU copy vs waiting for GPU)
- Reduces sync points from dozens per frame to potentially zero

#### Test Results on Meruru DX

**Setup:**
```bash
# Files placed in game directory:
~/.steam/steam/steamapps/common/Atelier Meruru/d3d11.dll

# Launch options tested:
WINEDLLOVERRIDES="d3d11=n,b" DXVK_HUD=pipelines,fps %command%
```

**Results:**
- Fix successfully loads and hooks D3D11 functions ✓
- Attempts to create shadow staging buffers ✗
- **Fails with error 0x80070057 (E_INVALIDARG) ~15,000 times**
- No performance improvement observed
- Menu lag remains 2+ seconds

**Log Output:**
```
Loading d3d11.dll successful, entry points are:
D3D11CreateDevice             @ 0x6ffffa87f830
D3D11CreateDeviceAndSwapChain @ 0x6ffffa87f880
Hooking device 0x15f0058
ID3D11Device::CreateDeferredContext @ 0x6ffffa89c1d0 -> 0x6ffffc8e58a0
Hooking context 0x15f7ac0
ID3D11DeviceContext::CopyResource @ 0x6ffffa94a3b0 -> 0x6ffffc8e3950
...
Failed to map destination resource, hr 0x80070057
Failed to map destination resource, hr 0x80070057
[repeats ~15,000 times]
```

**Conclusion:** The fix's assumptions about resource parameters (format, size, CPU access flags, usage types) don't match what Arland DX's older engine uses. When it tries to create shadow buffers, the parameters are invalid for Arland's resources.

**Why This Matters for Our Approach:**

doitsujin's fix failed because it's **engine-specific** - it needs to create compatible staging resources, which requires matching the exact resource formats/flags of each engine version.

Our flush-filtering approach is **engine-agnostic** - it doesn't care what resource formats the game uses. We just intercept Flush() calls and ignore most of them. This should work across ALL Atelier games (Arland, Dusk, Sophie, Ryza) regardless of engine version differences.

### Critical Question: Why Didn't doitsujin Use Flush Filtering?

**This is a crucial question that challenges our entire approach.**

doitsujin (creator of DXVK, expert in D3D11/Vulkan) chose the complex shadow buffer approach. If flush filtering is "so simple," why didn't he use it?

#### The Answer: Different Problems Require Different Solutions

**Sophie 2/Ryza 3 (doitsujin's targets) - Confirmed READBACK operations:**

```cpp
// Sophie 2 code (from doitsujin's documentation):
ID3D11Buffer* stagingBuffer;
context->CopyResource(stagingBuffer, gpuResource);  // GPU → CPU (READBACK)
context->Map(stagingBuffer, 0, D3D11_MAP_READ_WRITE, 0, &mapped);  // CPU READS
ProcessDataOnCPU(mapped.pData);  // Game actually uses GPU results on CPU
context->Unmap(stagingBuffer, 0);
```

**Key indicator:** `D3D11_MAP_READ_WRITE` - the CPU is reading data that the GPU produced.

**Why flush filtering WON'T work for Sophie 2:**

```cpp
context->Draw(scene);
context->CopyResource(staging, renderTarget);
context->Flush();  // ❌ CAN'T SKIP! CPU needs this data
context->Map(staging, D3D11_MAP_READ, ...);
ProcessData(mapped.pData);  // Would get STALE DATA if flush was skipped!
```

The game **actually needs** those GPU results on the CPU. Filtering the flush would cause the CPU to read incomplete/old data, breaking the game.

**Why shadow buffers DO work for Sophie 2:**

doitsujin's approach maintains CPU-side copies that are always ready:
- GPU works asynchronously updating the real resource
- CPU reads from shadow copy immediately (no wait)
- Shadow is updated in background when GPU finishes
- Breaks the synchronization dependency

**Arland DX (our target) - ASSUMED write operations (NOT VERIFIED!):**

```cpp
// What we THINK Arland does (ASSUMPTION):
for (int i = 0; i < 1000; i++) {
    context->Map(uiTexture[i], 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    memcpy(mapped.pData, uiData[i], size);  // CPU writes TO GPU
    context->Unmap(uiTexture[i]);
    context->Flush();  // Possibly unnecessary?
}
```

**Key indicator (assumed):** `D3D11_MAP_WRITE_DISCARD` - the CPU is writing data to the GPU, not reading.

#### Three Possible Scenarios

**Scenario 1: Arland Only Has WRITE Operations**
- ✅ Flush filtering should work
- ❌ Shadow buffers unnecessary (no readbacks to optimize)
- 🤔 Why didn't doitsujin try this simpler approach?
  - Maybe he didn't think of it (unlikely given his expertise)
  - Maybe he was focused on Sophie 2's readback problem
  - Maybe Arland's failure (INVALIDARG) made him move on

**Scenario 2: Arland ALSO Has Readbacks**
```cpp
// Possible: Menu checks texture formats before loading?
for (int i = 0; i < 1000; i++) {
    Map(texture, MAP_READ_WRITE);  // Check existing format
    CheckFormat(mapped.pData);
    ConvertIfNeeded();
    Unmap();
    Flush();  // Necessary - was a readback!
}
```
- ❌ Flush filtering wouldn't work (would break like Sophie 2)
- ✅ Shadow buffers necessary (same problem as Sophie 2)
- ✅ Explains why doitsujin used the complex approach

**Scenario 3: Mixed Operations**
- Some writes (loading textures)
- Some reads (checking formats, GPU picking, etc.)
- Need hybrid approach or shadow buffers
- Flush filtering alone insufficient

#### Why This Changes Everything

**Our entire plan is based on UNVERIFIED assumptions:**

1. ❓ We assume Map operations are WRITE (not confirmed)
2. ❓ We assume no readbacks occur during menu lag (not confirmed)
3. ❓ We assume flushes are unnecessary (not confirmed)
4. ❓ We assume doitsujin's approach was wrong for Arland (may not be true)

**Phase 1 diagnosis is CRITICAL - not just helpful, but absolutely essential.**

If Arland has readback operations like Sophie 2, our flush filtering approach will fail just like it would fail for Sophie 2. doitsujin may have discovered this and that's why he chose shadow buffers.

#### What Phase 1 MUST Determine

```markdown
RenderDoc Analysis [BLOCKING - DO NOT PROCEED WITHOUT THIS]
├─ Capture frame during menu lag
├─ Examine EVERY Map() call:
│  ├─ D3D11_MAP_WRITE_DISCARD (1) → Write operation ✅
│  ├─ D3D11_MAP_WRITE (2) → Write operation ✅
│  ├─ D3D11_MAP_READ (3) → Read operation ❌
│  └─ D3D11_MAP_READ_WRITE (4) → Read operation ❌
├─ If ALL are writes → Flush filtering should work
├─ If ANY are reads → Need shadow buffers or abandon project
└─ Check what happens AFTER each Flush():
   └─ Does a readback follow? Then flush is necessary
```

**We cannot proceed to Phase 2-4 without confirming our assumptions.**

#### Revised Confidence Level

**Initial assumption:** "Flush filtering is simple and will work - 90% confident"

**After considering doitsujin's approach:** "Flush filtering might work IF operations are writes - 50% confident"

**The fact that doitsujin chose the complex approach suggests:**
- He likely had good reasons
- The problem may be more nuanced than we initially thought
- Shadow buffers may be necessary for Arland too
- Our "simple" approach may have been tried and failed

**However:** doitsujin's fix DID fail on Arland with INVALIDARG errors, so maybe he never got far enough to test flush filtering. We might still be on the right track.

#### Contingency Plans

**If Phase 1 shows readback operations:**

1. **Option A:** Abandon flush filtering, fix doitsujin's shadow buffer approach for Arland
   - Figure out correct resource parameters for Arland's older engine
   - More complex but proven approach
   - Higher chance of success

2. **Option B:** Hybrid approach
   - Filter flushes after write operations
   - Keep flushes before read operations
   - More complex logic but might work

3. **Option C:** Accept defeat
   - Document findings
   - Consider this a learning project
   - Some games just can't be fixed easily

**If Phase 1 shows ONLY write operations:**

✅ Proceed with flush filtering as planned - our approach is likely correct

### 2. Lily's Atelier Graphics Tweak

**Project:** https://steamcommunity.com/app/1152300/discussions/0/3345546664208090238/  
**Author:** Lily (Steam community member)  
**Target Games:** Dusk DX trilogy (Ayesha/Escha/Shallie - 2019)  
**Files:** Google Drive link in forum post

#### How It Works
- More aggressive approach than doitsujin's fix
- "Captures and prevents execution" of certain D3D11 calls
- Includes anti-stutter code
- Also adds SMAA antialiasing
- Requires multiple files: d3d11.dll, HLSL shaders, D3DCompiler_47.dll

#### Why It's Relevant
- Targets games chronologically between Arland DX (2018) and Sophie 2 (2022)
- Different engine iteration than either
- Author notes the anti-stutter code "doesn't just modify DX11 API calls, but actually captures them and prevents their execution"
- May have different approach worth studying

#### Test Status
Not yet tested on Arland DX games.

---

## What We Know Works

### Confirmed Diagnostic Findings

1. **Menu lag is NOT shader compilation**
   - DXVK_HUD shows pipeline counter barely changes during menu opens
   - DXVK_ASYNC=1 (async shader compilation) has no effect
   - Lag is consistent every time (shader compilation would improve after first compile)

2. **Menu lag IS a GPU sync stall**
   - FPS counter completely freezes (entire render thread blocked)
   - Happens at exactly the same point every menu open
   - Brief 8fps catch-up frame suggests GPU was ready but CPU was blocked
   - Pattern matches doitsujin's description of GPU sync issues

3. **doitsujin's fix identifies the right problem but wrong solution**
   - Successfully hooks into the D3D11 API
   - Correctly identifies CopyResource as the problem
   - Fails to create compatible shadow resources for Arland DX

4. **The problem is reproducible and measurable**
   - Consistent 2-second stall
   - Can be captured with profiling tools
   - Can be debugged with DirectX debugging tools

---

## Technical Stack & Tools

### Current Setup
- **Game:** Atelier Meruru ~The Apprentice of Arland~ DX (Steam)
- **OS:** Pop!_OS Linux (Ubuntu 24.04 based)
- **Steam Proton:** Latest (includes Wine + DXVK)
- **Graphics API:** DirectX 11 (game) → Vulkan (DXVK) → GPU

### Development Tools Required

#### Diagnosis Phase
- **RenderDoc** - DirectX frame capture and analysis (https://renderdoc.org/)
  - Can capture D3D11 API calls during menu lag
  - Shows exact sequence of operations causing stall
  - Works through Wine/Proton

- **DXVK Debug Logging**
  ```bash
  DXVK_LOG_LEVEL=debug
  DXVK_HUD=full
  ```

- **mangohud** - Performance overlay for Vulkan/OpenGL
  ```bash
  mangohud %command%
  ```

- **perf** - Linux system profiler
  ```bash
  perf record -g steam steam://rungameid/936160
  ```

#### Development Phase
- **C++ Compiler** - gcc/clang (already available on Pop!_OS)
- **MinGW-w64** - Cross-compiler for Windows DLLs from Linux
- **Visual Studio Code** - IDE with C++ extensions
- **Git** - Version control

#### Debugging Phase
- **x64dbg** - Windows debugger (runs under Wine)
- **Ghidra** - Reverse engineering tool (optional, for deep analysis)
- **Wine debugger** - winedbg for debugging under Proton

### Key Repositories
- **DXVK source:** https://github.com/doitsujin/dxvk
- **atelier-sync-fix source:** https://github.com/doitsujin/atelier-sync-fix
- **Wine source:** https://gitlab.winehq.org/wine/wine

---

## Project Phases

### Phase 1: Detailed Diagnosis (2-4 weeks) ⚠️ **BLOCKING - CRITICAL**

**Goal:** Determine if flush filtering will work or if we need shadow buffers

**⚠️ CRITICAL IMPORTANCE:** Our entire approach (flush filtering vs shadow buffers) depends on what we discover here. We cannot proceed to implementation without this data.

**Skills Required:** None (learning as we go)

**Tasks:**
1. Install and configure RenderDoc on Pop!_OS
2. Launch Meruru DX through RenderDoc
3. Capture a frame during menu open freeze
4. **CRITICAL ANALYSIS** - Analyze the captured frame:
   - **PRIMARY QUESTION:** Are Map() operations READ or WRITE?
     - Count D3D11_MAP_WRITE_DISCARD operations (writes)
     - Count D3D11_MAP_WRITE operations (writes)
     - Count D3D11_MAP_READ operations (reads)
     - Count D3D11_MAP_READ_WRITE operations (reads)
   - **SECONDARY QUESTION:** What happens after Flush() calls?
     - Does a Map(READ) follow the Flush()?
     - Does the game use the mapped data?
   - Identify all CopyResource calls
   - Check resource descriptions (format, usage, CPU access flags)
   - Look for patterns/repeated operations
5. Enable verbose DXVK logging and correlate with RenderDoc findings
6. Document the exact sequence that causes stall

**Deliverable:** Technical document describing:
- **DECISION: Can we use flush filtering or do we need shadow buffers?**
- Exact D3D11 call sequence during menu lag
- Resource types and parameters being used
- Breakdown of READ vs WRITE operations
- Whether flushes are followed by readbacks
- Why doitsujin's fix fails (parameter mismatch specifics)
- Recommended approach based on findings

**GO/NO-GO Decision Point:**
- ✅ **GO:** If operations are primarily WRITE → Proceed with flush filtering (Phase 2-4)
- ⚠️ **PIVOT:** If operations include READS → Switch to shadow buffer approach
- ❌ **NO-GO:** If shadow buffers also fail → Document and accept defeat

### Phase 2: C++ Fundamentals (4-6 weeks)
**Goal:** Learn enough C++ to understand and modify DirectX hook code

**Skills Required:** Python experience (transferable concepts)

**Learning Path:**
1. **Basic C++ syntax** (1-2 weeks)
   - Variables, types, pointers, references
   - Functions, classes, structs
   - Memory management (new/delete)
   - Differences from Python

2. **DirectX 11 basics** (2-3 weeks)
   - COM interfaces (how D3D11 objects work)
   - Resource types (buffers, textures, render targets)
   - Pipeline stages
   - Map/Unmap, CopyResource operations
   - Resource descriptions and creation

3. **DLL hooking/injection** (1-2 weeks)
   - How wrapper DLLs work
   - Function pointer tables (vtables)
   - Hooking D3D11 interfaces
   - Forwarding calls to real implementation

**Resources:**
- Microsoft D3D11 documentation
- doitsujin's atelier-sync-fix source code (as reference)
- LearnCpp.com for C++ fundamentals
- CheatEngine forums for hooking techniques

**Practice Project:**
Create a simple D3D11 hook DLL that:
- Loads successfully
- Hooks CreateDevice
- Logs basic information
- Doesn't crash the game

### Phase 3: Build Minimal Working Hook (4-8 weeks)
**Goal:** Create a "Hello World" version that proves we can intercept and modify behavior

**Tasks:**
1. Set up MinGW-w64 cross-compilation environment
2. Fork doitsujin's atelier-sync-fix repository
3. Strip down to minimal working example
4. Modify to compile on Linux for Windows target
5. Test that minimal hook loads in Meruru DX
6. Add logging for the specific calls we identified in Phase 1
7. Verify we're seeing the expected operations

**Deliverable:** 
- Custom d3d11.dll that loads in Meruru DX
- Logs the problematic CopyResource/Map calls
- Doesn't break the game
- Provides detailed diagnostics

### Phase 4: Implement Fix (2-4 weeks)
**Goal:** Implement flush filtering to eliminate unnecessary GPU sync points

**Approach: Simple Flush Filtering (Not Shadow Buffers, Not Command Batching)**

This is simpler than doitsujin's shadow buffer approach and doesn't require the complex command batching described in the tutorial Phase 3 (see `docs/advanced_command_batching.md` for that complexity - we're NOT doing it).

**What We're Actually Implementing:**
```cpp
class FlushFilteringWrapper : public ID3D11DeviceContext {
private:
    ID3D11DeviceContext* real;
    int flushCount = 0;

public:
    // Forward everything immediately - no buffering!
    virtual void Draw(UINT count, UINT start) override {
        real->Draw(count, start);  // Immediate forwarding
    }

    virtual HRESULT Map(ID3D11Resource* res, UINT subres, D3D11_MAP type,
                       UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped) override {
        // Only flush if this is a readback
        if (type == D3D11_MAP_READ || type == D3D11_MAP_READ_WRITE) {
            Log("Readback detected - flushing");
            real->Flush();
        }

        return real->Map(res, subres, type, flags, mapped);
    }

    virtual void Flush() override {
        flushCount++;

        // Ignore 99% of flush calls (safety valve: allow 1%)
        if (flushCount % 100 == 0) {
            Log("Allowing flush #%d", flushCount);
            real->Flush();
        } else {
            Log("Ignoring flush #%d", flushCount);
            // Don't flush - this is the key optimization!
        }
    }

    // Forward everything else immediately
    // ... ~150 more simple forwarding methods
};
```

**Key Points:**
- ✅ All Map/Draw/Set* calls forwarded immediately (no command buffering)
- ✅ No need to intercept Present() (DXVK handles flushing automatically)
- ✅ No resource lifetime management complexity
- ✅ No dependency tracking needed
- ✅ Simple and low-risk

**Iterative Process:**
1. Get basic wrapper loading and forwarding all methods
2. Add logging to Map() and Flush() to confirm they're being called
3. Implement flush filtering logic
4. Test - measure menu lag reduction
5. Tune the flush filtering heuristics if needed
6. Test for visual glitches
7. Test across different game scenarios (menus, combat, cutscenes)

**Success Criteria:**
- Menu lag reduced from 2 seconds to <300ms (acceptable)
- Stretch goal: <100ms (excellent)
- No visual glitches
- No crashes
- Stable across multiple play sessions
- Works in all three Arland games

**Why This Works:**
Menu lag is caused by 1000 unnecessary `Flush()` calls between write operations. By ignoring 99% of them, we eliminate 990 unnecessary GPU sync points. The game's write operations still execute immediately (forwarded to real context), but without the blocking synchronization.

**What We're NOT Doing:**
- ❌ Shadow staging buffers (doitsujin's approach - doesn't work for Arland)
- ❌ Command batching (see `docs/advanced_command_batching.md` - too complex, unnecessary)
- ❌ Intercepting Present() (not needed for flush filtering)
- ❌ Resource dependency tracking (not needed for flush filtering)

### Phase 5: Polish & Release (2-4 weeks)
**Goal:** Make the fix production-ready and shareable

**Tasks:**
1. Test on all three Arland DX games (Rorona, Totori, Meruru)
2. Test on Windows (may need separate build)
3. Add configuration options (if needed)
4. Write comprehensive README
5. Create GitHub repository
6. Package releases for easy installation
7. Write installation guide for both Windows and Linux
8. Share on Steam forums, Reddit (r/Atelier, r/JRPG)
9. Consider submitting to PCGamingWiki

**Platform Compatibility:**
- **Linux/Proton:** d3d11.dll wrapper (our initial target)
- **Windows Native:** Same approach should work, may need recompilation
- **Potential DXVK Integration:** If fix is robust, could submit PR to DXVK to detect and optimize this pattern automatically

---

## Resources & Links

### Game Information
- **Steam Store:** https://store.steampowered.com/app/936160/ (Rorona)
- **PCGamingWiki:** https://www.pcgamingwiki.com/wiki/Atelier_Rorona:_The_Alchemist_of_Arland_DX
- **Atelier Wiki:** https://atelier.fandom.com/wiki/Atelier_Rorona:_The_Alchemist_of_Arland

### Existing Fix Projects
- **doitsujin atelier-sync-fix:** https://github.com/doitsujin/atelier-sync-fix
- **Lily's Graphics Tweak:** https://steamcommunity.com/app/1152300/discussions/0/3345546664208090238/

### Technical Documentation
- **DirectX 11 API Reference:** https://learn.microsoft.com/en-us/windows/win32/direct3d11/
- **DXVK Documentation:** https://github.com/doitsujin/dxvk/wiki
- **Proton Documentation:** https://github.com/ValveSoftware/Proton/wiki
- **RenderDoc Documentation:** https://renderdoc.org/docs/

### Community Resources
- **ProtonDB (Atelier):** https://www.protondb.com/app/936160
- **r/Atelier:** https://reddit.com/r/Atelier
- **Atelier Discord:** (various community servers)

---

## Known Challenges

### Technical
1. **Cross-platform compatibility:** Linux development for Windows target
2. **Resource parameter complexity:** Many different resource types to handle
3. **Potential edge cases:** Unusual rendering scenarios
4. **Memory management:** Shadow buffers could use significant RAM
5. **Wine/Proton quirks:** Behavior differences from native Windows

### Practical
1. **Time commitment:** 3-6 months estimated
2. **Learning curve:** C++, DirectX, low-level graphics programming
3. **Debugging difficulty:** Graphics bugs can be hard to diagnose
4. **No guarantee of success:** May encounter insurmountable obstacles
5. **Gust could patch:** (unlikely given their "working as intended" stance)

### Scope Risks
1. **Scope creep:** Temptation to fix other issues or add features
2. **Perfectionism:** Getting stuck trying to optimize too much
3. **Multiple games:** Supporting all three Arland games adds complexity
4. **Windows version:** May need separate development track

---

## Success Metrics

### Minimum Viable Product (MVP)
- Menu lag reduced from 2+ seconds to <0.5 seconds
- Works reliably on Meruru DX on Linux/Proton
- No crashes or visual glitches
- Open source and documented

### Stretch Goals
- Lag reduced to <0.1 seconds (imperceptible)
- Works on all three Arland DX games
- Works on both Linux and Windows
- Zero visual artifacts
- Negligible CPU/memory overhead
- Upstreamed to DXVK (automatic optimization)

---

## Next Immediate Steps

1. **Install RenderDoc:**
   ```bash
   sudo apt install renderdoc
   ```

2. **Capture a frame during menu lag:**
   - Launch Meruru DX through RenderDoc
   - Trigger menu lag
   - Capture the frame
   - Analyze D3D11 calls

3. **Document findings:**
   - What resources are being copied?
   - What are their descriptions?
   - How many sync points occur?
   - What parameters does doitsujin's fix try vs what's needed?

4. **Create GitHub repository:**
   - Fork atelier-sync-fix or start fresh
   - Document architecture
   - Begin with diagnosis phase code/notes

5. **Set up development environment:**
   - Install MinGW-w64
   - Configure VS Code for C++
   - Test basic DLL compilation

---

## Project Status

**Current Phase:** Phase 1 - RenderDoc Diagnosis (BLOCKING)
**Test Platform:** Atelier Meruru ~The Apprentice of Arland~ DX
**Approach:** Flush filtering (PENDING VERIFICATION)
**Confidence Level:** 50% (was 90%, revised after considering doitsujin's approach)
**Last Updated:** 2025-10-24

### Completed
- ✅ Identified that doitsujin's fix doesn't work (error 0x80070057)
- ✅ Confirmed menu lag is GPU sync, not shader compilation
- ✅ Established that the problem is reproducible and measurable
- ✅ Documented existing fix attempts and why they fail
- ✅ Analyzed why doitsujin chose shadow buffers over flush filtering
- ✅ Identified critical assumption: operations must be WRITES, not READS
- ✅ Recognized Phase 1 as blocking decision point

### In Progress (CRITICAL)
- ⚠️ **BLOCKING:** Phase 1 RenderDoc diagnosis
  - Must determine if Map() operations are READ or WRITE
  - This determines if flush filtering will work at all
  - Cannot proceed to implementation without this data

### Upcoming (Conditional on Phase 1 Results)
- ⏳ **IF WRITES:** Install and configure RenderDoc
- ⏳ **IF WRITES:** Capture and analyze frame during menu lag
- ⏳ **IF WRITES:** Confirm flush filtering is viable
- ⏳ **IF WRITES:** Begin C++ learning path
- ⏳ **IF WRITES:** Implement flush filtering wrapper
- ⚠️ **IF READS:** Pivot to shadow buffer approach OR accept defeat

---

## Notes & Observations

- The fact that doitsujin's fix loads but fails with INVALIDARG is actually good news - it means the approach is sound, but the implementation needs tailoring
- Lily's fix for Dusk games suggests this is a recurring problem across multiple Atelier engine iterations
- Gust's "working as intended" response suggests they're either unaware of the severity or unwilling to fix legacy code
- The consistency of the 2-second lag makes it an ideal target for optimization - it's not random, it's a specific code path
- This project could benefit the entire Atelier community if successful
- Even if we can't eliminate lag entirely, reducing it to <0.3s would still be a massive improvement

### Key Insight: Why Phase 1 is Critical

**Initial overconfidence:** We initially assumed flush filtering would work because "menu opening = loading assets = write operations."

**Reality check:** doitsujin (DXVK creator) chose the complex shadow buffer approach for Sophie 2. Why?
- Sophie 2 has confirmed READBACK operations (CPU reads GPU results)
- Flush filtering won't work for readbacks - the flushes are necessary
- Shadow buffers break the sync dependency by maintaining CPU-side copies

**The question:** Does Arland also have readbacks, or is it purely writes?
- If WRITES only → Flush filtering should work ✅
- If READS present → Need shadow buffers or different approach ❌
- We won't know until we capture with RenderDoc

**Revised approach:** Treat Phase 1 as a GO/NO-GO decision point, not just "nice to have" diagnostics. The entire project depends on what we discover.

---

**Philosophy:** Better to release something 80% optimal that works than to chase 100% perfection forever.

---

*"The reasonable man adapts himself to the world; the unreasonable one persists in trying to adapt the world to himself. Therefore all progress depends on the unreasonable man."* - George Bernard Shaw
