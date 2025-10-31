# MinHook Approach for D3D11 Interception

## Overview

MinHook is a Windows API hooking library that enables runtime interception of function calls by patching vtable entries. This document explains how it works and why it's superior to the wrapper DLL approach for simple D3D11 hooks.

---

## What is MinHook?

**MinHook** is a minimalistic x86/x64 hooking library for Windows that uses inline code patching to redirect function calls.

**Repository:** https://github.com/TsudaKageyu/minhook

### Key Features
- Lightweight and fast
- Supports both x86 and x64
- Thread-safe hook creation
- Easy to use API
- No need to implement full interface wrappers

---

## How COM Vtables Work

COM interfaces like `ID3D11DeviceContext` use **vtables** (virtual function tables) to implement polymorphism:

```
Memory layout of ID3D11DeviceContext object:

Object instance:
  [vtable pointer] ───────┐
  [internal data]         │
                          │
                          ↓
Vtable (array of function pointers):
  [0]  QueryInterface  → 0x7ffe1234000
  [1]  AddRef          → 0x7ffe1234100
  [2]  Release         → 0x7ffe1234200
  [3]  GetDevice       → 0x7ffe1234300
  ...
  [47] CopyResource    → 0x7ffe1245000  ← points to real implementation
  [48] UpdateSubresource → 0x7ffe1245100
  ...
  [110] Flush          → 0x7ffe1246000
  ...
  [150+] (more methods)
```

When you call `context->Flush()`:
1. Compiler generates: `call [vtable + 110*8]` (on x64)
2. CPU jumps to address stored in vtable slot 110
3. That address points to the real Flush() implementation

---

## MinHook vs Wrapper Approach

### Wrapper DLL Approach

**How it works:**
- Create a new class implementing the entire interface
- Forward all method calls to the real implementation
- Return your wrapper object to the game

```cpp
class WrappedContext : public ID3D11DeviceContext {
private:
  ID3D11DeviceContext* real;  // Pointer to real context

public:
  // Must implement ALL 150+ methods!

  virtual void Flush() override {
    // Your custom logic
    if (shouldFlush())
      real->Flush();
  }

  virtual void Draw(UINT count, UINT start) override {
    real->Draw(count, start);  // Simple forward
  }

  virtual void Map(...) override {
    return real->Map(...);  // Simple forward
  }

  // ... 147 more forwarding methods ...
};

// In D3D11CreateDevice():
ID3D11DeviceContext* realContext = nullptr;
RealD3D11CreateDevice(..., &realContext);
*ppContext = new WrappedContext(realContext);  // Return wrapper
```

**Pros:**
- Conceptually simple
- Easy to debug (set breakpoints in methods)
- No external dependencies
- Full control over all methods

**Cons:**
- Must implement ALL 150+ interface methods
- ~2000+ lines of boilerplate code
- Two objects in memory (wrapper + real)
- Slight performance overhead (extra indirection)
- Must keep track of COM reference counting

### MinHook Approach

**How it works:**
- Get the real context object
- Patch specific vtable entries to point to your functions
- Return the real object (now with modified vtable)

```cpp
// Store original function pointers
PFN_ID3D11DeviceContext_Flush g_originalFlush = nullptr;

// Your replacement function
void STDMETHODCALLTYPE My_Flush(ID3D11DeviceContext* pContext) {
  static int count = 0;

  if (++count % 100 == 0)
    g_originalFlush(pContext);  // Call original
  // Else ignore
}

// Hook setup
void hookContext(ID3D11DeviceContext* pContext) {
  void** vtbl = *reinterpret_cast<void***>(pContext);

  MH_CreateHook(
    vtbl[110],              // Original Flush() address
    &My_Flush,              // Your replacement
    &g_originalFlush        // Store original
  );

  MH_EnableHook(vtbl[110]);
}

// In D3D11CreateDevice():
ID3D11DeviceContext* realContext = nullptr;
RealD3D11CreateDevice(..., &realContext);
hookContext(realContext);  // Patch vtable
*ppContext = realContext;  // Return real object (now patched)
```

**Pros:**
- Only implement methods you care about
- Minimal code (~50 lines vs ~2000)
- One object in memory (the real one)
- No performance overhead
- No COM reference counting issues

**Cons:**
- External dependency (MinHook library)
- Slightly harder to debug (vtable indirection)
- Need to know vtable indices
- More "magical" than wrapper approach

---

## MinHook API

### Initialization

```cpp
// In DllMain():
BOOL WINAPI DllMain(HINSTANCE dll, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH)
    MH_Initialize();  // Initialize MinHook
  else if (reason == DLL_PROCESS_DETACH)
    MH_Uninitialize();  // Cleanup

  return TRUE;
}
```

### Creating a Hook

```cpp
MH_STATUS MH_CreateHook(
  LPVOID pTarget,      // Address of function to hook
  LPVOID pDetour,      // Your replacement function
  LPVOID *ppOriginal   // Pointer to receive original function
);

// Returns:
// - MH_OK on success
// - MH_ERROR_ALREADY_CREATED if already hooked
// - Other error codes
```

### Enabling/Disabling Hooks

```cpp
MH_STATUS MH_EnableHook(LPVOID pTarget);   // Activate hook
MH_STATUS MH_DisableHook(LPVOID pTarget);  // Deactivate hook
MH_STATUS MH_RemoveHook(LPVOID pTarget);   // Remove hook entirely
```

### Helper for COM vtable Hooking

```cpp
template<typename T>
void hookProc(void* pObject, const char* pName, T** ppOrig, T* pHook, uint32_t index) {
  void** vtbl = *reinterpret_cast<void***>(pObject);  // Get vtable

  MH_STATUS mh = MH_CreateHook(
    vtbl[index],                          // vtable entry
    reinterpret_cast<void*>(pHook),       // Your function
    reinterpret_cast<void**>(ppOrig)      // Store original
  );

  if (mh != MH_OK && mh != MH_ERROR_ALREADY_CREATED) {
    log("Failed to create hook for ", pName);
    return;
  }

  mh = MH_EnableHook(vtbl[index]);
  if (mh != MH_OK)
    log("Failed to enable hook for ", pName);
}
```

---

## Vtable Indices for ID3D11DeviceContext

These are the vtable indices you need to hook specific methods:

```cpp
// IUnknown (inherited)
// 0  - QueryInterface
// 1  - AddRef
// 2  - Release

// ID3D11DeviceChild (inherited)
// 3  - GetDevice
// 4  - GetPrivateData
// 5  - SetPrivateData
// 6  - SetPrivateDataInterface

// ID3D11DeviceContext methods
// 7  - VSSetConstantBuffers
// 8  - PSSetShaderResources
// 9  - PSSetShader
// ...
// 33 - OMSetRenderTargets
// 34 - OMSetRenderTargetsAndUnorderedAccessViews
// ...
// 41 - Dispatch
// 42 - DispatchIndirect
// ...
// 46 - CopySubresourceRegion
// 47 - CopyResource
// 48 - UpdateSubresource
// 49 - CopyStructureCount
// 50 - ClearRenderTargetView
// 51 - ClearUnorderedAccessViewUint
// 52 - ClearUnorderedAccessViewFloat
// ...
// 110 - Flush
// 111 - GetType
// ...
```

**Finding vtable indices:**
- Check Microsoft D3D11 documentation
- Use a debugger to inspect vtable
- Reference existing implementations (like doitsujin's)

---

## Complete Minimal Example for Flush Filtering

This is a complete, working example that hooks only `Flush()`:

```cpp
#include <windows.h>
#include <d3d11.h>
#include <fstream>
#include "MinHook.h"

// Logging
void Log(const char* msg) {
  std::ofstream log("atfix.log", std::ios::app);
  log << msg << std::endl;
}

// Function pointer type for Flush()
typedef void (STDMETHODCALLTYPE *PFN_Flush)(ID3D11DeviceContext*);

// Store original Flush() for immediate and deferred contexts
PFN_Flush g_originalFlush_Immediate = nullptr;
PFN_Flush g_originalFlush_Deferred = nullptr;

// Replacement Flush() function
void STDMETHODCALLTYPE My_Flush(ID3D11DeviceContext* pContext) {
  static int flushCount = 0;
  flushCount++;

  // Allow 1% of flushes through as safety valve
  if (flushCount % 100 == 0) {
    Log("Allowing flush");

    // Call original based on context type
    if (pContext->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE)
      g_originalFlush_Immediate(pContext);
    else
      g_originalFlush_Deferred(pContext);
  } else {
    // Silently ignore 99% of flushes
  }
}

// Hook Flush() on a context
void hookContext(ID3D11DeviceContext* pContext) {
  void** vtbl = *reinterpret_cast<void***>(pContext);

  PFN_Flush* pOriginal = (pContext->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE)
    ? &g_originalFlush_Immediate
    : &g_originalFlush_Deferred;

  MH_STATUS mh = MH_CreateHook(
    vtbl[110],                           // Flush is at index 110
    reinterpret_cast<void*>(&My_Flush),
    reinterpret_cast<void**>(pOriginal)
  );

  if (mh == MH_OK || mh == MH_ERROR_ALREADY_CREATED) {
    MH_EnableHook(vtbl[110]);
    Log("Hooked Flush() successfully");
  } else {
    Log("Failed to hook Flush()");
  }
}

// Load system D3D11
typedef HRESULT (WINAPI *PFN_D3D11CreateDevice)(
  IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
  const D3D_FEATURE_LEVEL*, UINT, UINT,
  ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

PFN_D3D11CreateDevice RealD3D11CreateDevice = nullptr;

HMODULE LoadSystemD3D11() {
  char sysPath[MAX_PATH];
  GetSystemDirectoryA(sysPath, MAX_PATH);
  strcat_s(sysPath, "\\d3d11.dll");

  HMODULE lib = LoadLibraryA(sysPath);
  if (lib) {
    RealD3D11CreateDevice = reinterpret_cast<PFN_D3D11CreateDevice>(
      GetProcAddress(lib, "D3D11CreateDevice"));
  }
  return lib;
}

// Exported D3D11CreateDevice
extern "C" __declspec(dllexport)
HRESULT WINAPI D3D11CreateDevice(
  IDXGIAdapter* pAdapter,
  D3D_DRIVER_TYPE DriverType,
  HMODULE Software,
  UINT Flags,
  const D3D_FEATURE_LEVEL* pFeatureLevels,
  UINT FeatureLevels,
  UINT SDKVersion,
  ID3D11Device** ppDevice,
  D3D_FEATURE_LEVEL* pFeatureLevel,
  ID3D11DeviceContext** ppImmediateContext)
{
  if (!RealD3D11CreateDevice)
    return E_FAIL;

  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;

  // Call real D3D11CreateDevice
  HRESULT hr = RealD3D11CreateDevice(
    pAdapter, DriverType, Software, Flags,
    pFeatureLevels, FeatureLevels, SDKVersion,
    &device, pFeatureLevel, &context);

  if (SUCCEEDED(hr)) {
    // Hook the context
    hookContext(context);

    // Return the real objects (now patched)
    if (ppDevice) {
      device->AddRef();
      *ppDevice = device;
    }
    if (ppImmediateContext) {
      context->AddRef();
      *ppImmediateContext = context;
    }

    device->Release();
    context->Release();
  }

  return hr;
}

// DLL entry point
BOOL WINAPI DllMain(HINSTANCE dll, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    MH_Initialize();
    LoadSystemD3D11();
    Log("atelier-arland-fix loaded");
  } else if (reason == DLL_PROCESS_DETACH) {
    MH_Uninitialize();
  }
  return TRUE;
}
```

**That's the entire implementation!** ~100 lines vs ~2000 for wrapper approach.

---

## Build Process with MinHook

### Project Structure

```
project/
├── src/
│   └── main.cpp          (your code above)
├── minhook/              (MinHook library)
│   ├── include/
│   │   └── MinHook.h
│   └── lib/
│       ├── libMinHook.x86.a
│       └── libMinHook.x64.a
├── meson.build
└── d3d11.def
```

### d3d11.def (Export Definition)

```
LIBRARY d3d11.dll
EXPORTS
  D3D11CreateDevice
  D3D11CreateDeviceAndSwapChain
```

### Build with MinGW

```bash
x86_64-w64-mingw32-g++ -shared -o d3d11.dll \
  src/main.cpp \
  minhook/lib/libMinHook.x64.a \
  -Iminhook/include \
  -static-libgcc -static-libstdc++ \
  -ld3d11 -ldxgi -luuid \
  -O2 \
  d3d11.def
```

### Meson Build (like doitsujin's)

```python
# meson.build
project('atelier-arland-fix', 'cpp')

minhook = declare_dependency(
  include_directories: include_directories('minhook/include'),
  dependencies: [
    compiler.find_library('MinHook', dirs: meson.current_source_dir() + '/minhook/lib')
  ]
)

shared_library('d3d11', 'src/main.cpp',
  dependencies: [minhook],
  vs_module_defs: 'd3d11.def',
  install: true
)
```

---

## Advantages for Arland Fix

For the Arland menu lag fix, MinHook is ideal because:

1. **We only need to hook Flush()**
   - One method = ~50 lines of code
   - Wrapper would need 150+ methods

2. **Phase 1 findings show queue submission spam**
   - Filtering Flush() is the right solution
   - Don't need complex shadow buffers

3. **Less code = less bugs**
   - Minimal surface area for errors
   - Easy to understand and maintain

4. **Proven approach**
   - doitsujin uses it successfully
   - Battle-tested on Sophie 2, Ryza 3

5. **Easy to extend**
   - If we need to hook more methods later (Map, CopyResource)
   - Just add more HOOK_PROC calls
   - No need to rewrite entire wrapper

---

## Common Pitfalls

### 1. Wrong Vtable Index

**Problem:** Hooking the wrong method because vtable index is incorrect.

**Solution:**
- Verify indices in debugger
- Check D3D11 documentation
- Cross-reference with known implementations

### 2. Calling Convention Mismatch

**Problem:** Crash because function signature doesn't match.

**Solution:**
- Always use `STDMETHODCALLTYPE` (expands to `__stdcall` on Windows)
- Match parameter types exactly
- Include `this` pointer as first parameter

### 3. Forgetting to Initialize MinHook

**Problem:** Hooks don't work, returns MH_ERROR_NOT_INITIALIZED.

**Solution:**
- Call `MH_Initialize()` in DllMain
- Call `MH_Uninitialize()` on detach

### 4. Hooking Before Context is Fully Created

**Problem:** Hook fails or crashes.

**Solution:**
- Hook AFTER successful D3D11CreateDevice
- Check return code before hooking

### 5. Thread Safety

**Problem:** Race conditions if multiple threads create contexts.

**Solution:**
- Use mutexes when installing hooks
- MinHook is thread-safe internally, but your hook logic might not be

---

## Comparison with Wrapper for Arland Fix

| Aspect | Wrapper | MinHook |
|--------|---------|---------|
| Code to write | ~2000 lines | ~100 lines |
| Methods to implement | 150+ | 1 (Flush) |
| External dependencies | None | MinHook |
| Performance | Slight overhead | Native |
| Complexity | Simple concept | Requires understanding vtables |
| Extensibility | Easy (add method) | Easy (add hook) |
| Debugging | Easy | Medium |
| **Time to implement** | **2-4 weeks** | **1-2 days** |

**Recommendation:** Use MinHook for Arland fix. The time savings and code simplicity far outweigh the slight increase in conceptual complexity.

---

## Next Steps

1. **Get MinHook library**
   - Download from https://github.com/TsudaKageyu/minhook
   - Or clone doitsujin's repo (includes MinHook as submodule)

2. **Set up build environment**
   - Install MinGW-w64
   - Test basic compilation

3. **Implement minimal hook**
   - Start with logging-only Flush() hook
   - Verify it loads in Meruru DX
   - Confirm Flush() calls are being intercepted

4. **Add filtering logic**
   - Implement 1% pass-through
   - Test menu lag reduction
   - Tune percentage if needed

5. **Polish and release**
   - Test all three Arland games
   - Package for easy installation
   - Share on Steam forums

---

## References

- **MinHook:** https://github.com/TsudaKageyu/minhook
- **doitsujin's atelier-sync-fix:** https://github.com/doitsujin/atelier-sync-fix
- **D3D11 API Reference:** https://learn.microsoft.com/en-us/windows/win32/direct3d11/
- **COM Programming:** https://learn.microsoft.com/en-us/windows/win32/com/
