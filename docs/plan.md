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

### Root Cause (Suspected)
GPU synchronization stalls caused by poor DirectX 11 resource management in Gust's engine. The game performs blocking CPU-GPU synchronization when copying resources for menu rendering, causing the entire render thread to freeze while waiting for the GPU.

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

### Phase 1: Detailed Diagnosis (2-4 weeks)
**Goal:** Understand exactly what D3D11 calls cause the lag and what resource types are involved

**Skills Required:** None (learning as we go)

**Tasks:**
1. Install and configure RenderDoc on Pop!_OS
2. Launch Meruru DX through RenderDoc
3. Capture a frame during menu open freeze
4. Analyze the captured frame:
   - Identify all CopyResource calls
   - Identify all Map/Unmap calls
   - Check resource descriptions (format, usage, CPU access flags)
   - Look for patterns/repeated operations
5. Enable verbose DXVK logging and correlate with RenderDoc findings
6. Document the exact sequence that causes stall

**Deliverable:** Technical document describing:
- Exact D3D11 call sequence during menu lag
- Resource types and parameters being used
- Why doitsujin's fix fails (parameter mismatch specifics)
- What parameters would work

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

### Phase 4: Implement Fix (Time Unknown)
**Goal:** Create shadow staging buffers with correct parameters for Arland DX resources

**Approach:**
Based on Phase 1 findings, create shadow buffers that:
- Match Arland DX's resource formats exactly
- Use compatible CPU access flags
- Handle the specific resource types the game uses
- Update shadows at the right time
- Intercept the blocking Map calls and return shadow data instantly

**Iterative Process:**
1. Implement shadow buffer creation for one resource type
2. Test - measure if lag reduces
3. Debug issues (likely many)
4. Repeat for each resource type causing stalls
5. Optimize memory usage
6. Optimize CPU overhead

**Success Criteria:**
- Menu lag reduced from 2 seconds to <0.2 seconds
- No visual glitches
- Stable across multiple play sessions
- No memory leaks

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

**Current Phase:** Phase 1 - Detailed Diagnosis  
**Test Platform:** Atelier Meruru ~The Apprentice of Arland~ DX  
**Last Updated:** 2025-10-22

### Completed
- ✅ Identified that doitsujin's fix doesn't work (error 0x80070057)
- ✅ Confirmed menu lag is GPU sync, not shader compilation
- ✅ Established that the problem is reproducible and measurable
- ✅ Documented existing fix attempts and why they fail

### In Progress
- 🔄 Setting up RenderDoc for frame capture
- 🔄 Detailed diagnosis of exact D3D11 call sequence

### Upcoming
- ⏳ Install and configure RenderDoc
- ⏳ Capture and analyze frame during menu lag
- ⏳ Document resource types and parameters
- ⏳ Begin C++ learning path

---

## Notes & Observations

- The fact that doitsujin's fix loads but fails with INVALIDARG is actually good news - it means the approach is sound, but the implementation needs tailoring
- Lily's fix for Dusk games suggests this is a recurring problem across multiple Atelier engine iterations
- Gust's "working as intended" response suggests they're either unaware of the severity or unwilling to fix legacy code
- The consistency of the 2-second lag makes it an ideal target for optimization - it's not random, it's a specific code path
- This project could benefit the entire Atelier community if successful
- Even if we can't eliminate lag entirely, reducing it to <0.3s would still be a massive improvement

---

**Philosophy:** Better to release something 80% optimal that works than to chase 100% perfection forever.

---

*"The reasonable man adapts himself to the world; the unreasonable one persists in trying to adapt the world to himself. Therefore all progress depends on the unreasonable man."* - George Bernard Shaw
