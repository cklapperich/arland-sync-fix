# D3D11 API Reference for Wrapper Development

Quick reference for understanding D3D11 concepts and building API wrappers.

---

## ID3D11DeviceContext

The command queue object between CPU and GPU. All rendering operations go through it:

```cpp
// Drawing
context->Draw(100, 0);                    // Draw 100 vertices
context->DrawIndexed(300, 0, 0);          // Draw 300 indexed vertices

// Resource updates
context->Map(buffer, ...);                // Lock resource for CPU access
context->Unmap(buffer);                   // Unlock resource
context->UpdateSubresource(texture, ...); // Update texture data
context->CopyResource(dest, src);         // Copy between resources

// State changes
context->PSSetShader(pixelShader, ...);   // Set pixel shader
context->IASetVertexBuffers(...);         // Set vertex buffers

// Synchronization
context->Flush();                         // Force GPU to start executing
```

**Key fact:** Commands are queued, not executed immediately. GPU and CPU run in parallel.

---

## Map() and Unmap()

### Map() Signature

```cpp
HRESULT Map(
    ID3D11Resource* resource,
    UINT subresource,
    D3D11_MAP mapType,        // Direction: read or write?
    UINT mapFlags,
    D3D11_MAPPED_SUBRESOURCE* mappedResource
);
```

### Map Types

**Write Operations (CPU → GPU) - No sync needed:**

```cpp
// D3D11_MAP_WRITE_DISCARD (1) - Completely overwrite resource
context->Map(texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
memcpy(mapped.pData, imageData, size);  // CPU writes to GPU memory
context->Unmap(texture);

// D3D11_MAP_WRITE (2) - Partial update
context->Map(buffer, 0, D3D11_MAP_WRITE, 0, &mapped);
((float*)mapped.pData)[0] = newValue;
context->Unmap(buffer);

// D3D11_MAP_WRITE_NO_OVERWRITE (3) - Append without overwriting
context->Map(buffer, 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapped);
memcpy(mapped.pData + offset, newData, size);
context->Unmap(buffer);
```

**Read Operations (GPU → CPU) - REQUIRES FLUSH:**

```cpp
// D3D11_MAP_READ (3) - Read from GPU
context->Draw(...);           // Render scene
context->Flush();             // MUST flush - GPU needs to finish!
context->Map(renderTarget, 0, D3D11_MAP_READ, 0, &mapped);
memcpy(screenshotData, mapped.pData, size);  // CPU reads from GPU
context->Unmap(renderTarget);

// D3D11_MAP_READ_WRITE (4) - Read and modify
context->Flush();  // MUST flush first!
context->Map(buffer, 0, D3D11_MAP_READ_WRITE, 0, &mapped);
float* data = (float*)mapped.pData;
data[0] = data[0] * 2.0f;  // Read, modify, write
context->Unmap(buffer);
```

---

## Flush()

Forces GPU to begin executing queued commands.

```cpp
// Without Flush - commands stay in queue
context->Draw(scene);     // Queued
context->Draw(ui);        // Queued
// ... GPU may not have started yet

// With Flush - commands execute now
context->Draw(scene);     // Queued
context->Flush();         // Execute NOW
// GPU guaranteed to be working on it
```

**When Flush() is necessary:**
- Before reading GPU results (MAP_READ)
- Before GetData() on queries
- At frame boundaries (though Present() does this implicitly)

**When Flush() is NOT necessary:**
- After write operations (MAP_WRITE)
- Between draw calls
- Most of the time

---

## Queries

Ask the GPU questions about rendering:

```cpp
// Create query
ID3D11Query* query;
D3D11_QUERY_DESC desc = { D3D11_QUERY_OCCLUSION, 0 };
device->CreateQuery(&desc, &query);

// Measure something
context->Begin(query);
context->Draw(object);  // How many pixels drawn?
context->End(query);

// Get results (this is a readback!)
UINT64 pixelsDrawn;
context->GetData(query, &pixelsDrawn, sizeof(pixelsDrawn), 0);
```

**GetData() always requires synchronization** - it's reading GPU results.

---

## The Wrapper Pattern

### Basic Concept

```cpp
class WrappedContext : public ID3D11DeviceContext {
private:
    ID3D11DeviceContext* realContext;  // Pointer to real implementation

public:
    // Most methods: simple forward
    virtual void Draw(UINT count, UINT start) override {
        realContext->Draw(count, start);  // One line - just call real thing
    }

    // Methods you want to modify: add logic
    virtual void Flush() override {
        // Your custom logic here
        if (shouldIgnore()) {
            return;  // Skip the real call
        }
        realContext->Flush();  // Call real implementation
    }
};
```

### Complete Minimal Example

```cpp
#include <windows.h>
#include <d3d11.h>
#include <fstream>

void Log(const char* msg) {
    std::ofstream log("d3d11_log.txt", std::ios::app);
    log << msg << std::endl;
}

// Load real D3D11
typedef HRESULT (WINAPI *PFN_D3D11CreateDevice)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT,
    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

PFN_D3D11CreateDevice RealD3D11CreateDevice = nullptr;

class WrappedContext : public ID3D11DeviceContext {
private:
    ID3D11DeviceContext* real;
    ULONG refCount;

public:
    WrappedContext(ID3D11DeviceContext* r) : real(r), refCount(1) {}

    // IUnknown (required for COM)
    virtual ULONG STDMETHODCALLTYPE AddRef() override {
        return ++refCount;
    }

    virtual ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = --refCount;
        if (count == 0) {
            real->Release();
            delete this;
        }
        return count;
    }

    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** obj) override {
        return real->QueryInterface(riid, obj);
    }

    // ID3D11DeviceChild (required)
    virtual void STDMETHODCALLTYPE GetDevice(ID3D11Device** device) override {
        real->GetDevice(device);
    }

    virtual HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override {
        return real->GetPrivateData(guid, size, data);
    }

    virtual HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override {
        return real->SetPrivateData(guid, size, data);
    }

    virtual HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* data) override {
        return real->SetPrivateDataInterface(guid, data);
    }

    // ID3D11DeviceContext - example methods
    virtual void STDMETHODCALLTYPE Draw(UINT count, UINT start) override {
        real->Draw(count, start);
    }

    virtual void STDMETHODCALLTYPE Flush() override {
        static int flushCount = 0;
        Log("Flush called");

        // Example: ignore 99% of flushes
        if (flushCount++ % 100 == 0) {
            real->Flush();
        }
    }

    virtual HRESULT STDMETHODCALLTYPE Map(
        ID3D11Resource* resource,
        UINT subresource,
        D3D11_MAP mapType,
        UINT flags,
        D3D11_MAPPED_SUBRESOURCE* mapped) override {

        // Detect readbacks
        if (mapType == D3D11_MAP_READ || mapType == D3D11_MAP_READ_WRITE) {
            Log("Readback detected - flushing");
            real->Flush();
        }

        return real->Map(resource, subresource, mapType, flags, mapped);
    }

    virtual void STDMETHODCALLTYPE Unmap(ID3D11Resource* resource, UINT subresource) override {
        real->Unmap(resource, subresource);
    }

    // ... ~146 more methods - all simple forwards like Draw()
};

// DLL entry point
BOOL WINAPI DllMain(HINSTANCE dll, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        Log("DLL loaded");

        // Load real d3d11.dll from system directory
        char sysPath[MAX_PATH];
        GetSystemDirectoryA(sysPath, MAX_PATH);
        strcat_s(sysPath, "\\d3d11.dll");

        HMODULE realD3D11 = LoadLibraryA(sysPath);
        if (realD3D11) {
            RealD3D11CreateDevice = (PFN_D3D11CreateDevice)
                GetProcAddress(realD3D11, "D3D11CreateDevice");
        }
    }
    return TRUE;
}

// Exported function
extern "C" __declspec(dllexport)
HRESULT WINAPI D3D11CreateDevice(
    IDXGIAdapter* adapter,
    D3D_DRIVER_TYPE driverType,
    HMODULE software,
    UINT flags,
    const D3D_FEATURE_LEVEL* featureLevels,
    UINT numFeatureLevels,
    UINT sdkVersion,
    ID3D11Device** device,
    D3D_FEATURE_LEVEL* featureLevel,
    ID3D11DeviceContext** context)
{
    Log("D3D11CreateDevice intercepted");

    // Call real function
    ID3D11DeviceContext* realContext = nullptr;
    HRESULT hr = RealD3D11CreateDevice(
        adapter, driverType, software, flags,
        featureLevels, numFeatureLevels, sdkVersion,
        device, featureLevel, &realContext);

    if (SUCCEEDED(hr) && context) {
        // Return wrapped context
        *context = new WrappedContext(realContext);
    }

    return hr;
}
```

### Build Command (MinGW)

```bash
x86_64-w64-mingw32-g++ -shared -o d3d11.dll wrapper.cpp \
    -static-libgcc -static-libstdc++ \
    -ld3d11 -ldxgi -luuid \
    -O2
```

---

## Resource Lifecycle (COM)

D3D11 uses COM reference counting:

```cpp
// Creating a resource
ID3D11Buffer* buffer;
device->CreateBuffer(&desc, nullptr, &buffer);  // refCount = 1

// Using in a command
context->IASetVertexBuffers(0, 1, &buffer, ...);
// Driver internally does: buffer->AddRef() [refCount = 2]

// Release your reference
buffer->Release();  // refCount = 1
// Driver still has reference, resource still alive

// When GPU finishes using it
// Driver internally does: buffer->Release() [refCount = 0]
// Resource deleted
```

You don't need to manage refcounts when forwarding calls - the driver handles it.

---

## Common Patterns

### Detecting Operation Type

```cpp
virtual HRESULT Map(..., D3D11_MAP mapType, ...) override {
    switch (mapType) {
        case D3D11_MAP_WRITE_DISCARD:     // 1
        case D3D11_MAP_WRITE:              // 2
        case D3D11_MAP_WRITE_NO_OVERWRITE: // 3
            // Write operation - no sync needed
            break;

        case D3D11_MAP_READ:               // 4
        case D3D11_MAP_READ_WRITE:         // 5
            // Read operation - MUST sync!
            real->Flush();
            break;
    }

    return real->Map(resource, subresource, mapType, flags, mapped);
}
```

### Filtering Unnecessary Calls

```cpp
virtual void Flush() override {
    static int count = 0;
    count++;

    // Allow 1% through as safety valve
    if (count % 100 == 0) {
        real->Flush();
    }
    // Ignore the other 99%
}
```

### Adding Logging

```cpp
virtual void Draw(UINT count, UINT start) override {
    Log("Draw: %d vertices from %d", count, start);
    real->Draw(count, start);
}
```

---

## Important Facts

1. **You're not reimplementing D3D11** - you're just forwarding calls to the real implementation
2. **Most methods are one-line forwards** - only 2-3 need special logic
3. **Map() is NOT always a read** - check the mapType parameter
4. **Flush() is often unnecessary** - only needed before readbacks
5. **Present() automatically flushes** - DXGI handles this
6. **COM refcounting is automatic** - driver manages resource lifetimes
7. **All ~150 methods must be implemented** - even if just forwarding
8. **The wrapper pattern is like BepInEx** - intercept, modify, forward

---

## Further Reading

- **Microsoft D3D11 Docs:** https://learn.microsoft.com/en-us/windows/win32/direct3d11/
- **DXVK Source:** https://github.com/doitsujin/dxvk (real-world example)
- **COM Basics:** IUnknown interface, AddRef/Release pattern
