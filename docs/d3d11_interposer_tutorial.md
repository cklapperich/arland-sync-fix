# D3D11 Interposer Tutorial
## Building a Command Batching Wrapper for Performance Optimization

**Use Case:** Fix Atelier Meruru DX menu lag (1000 synchronous GPU submissions → batched execution)

---

## Table of Contents
1. [Core Concepts](#core-concepts)
2. [Understanding Map Operations](#understanding-map-operations)
3. [Readbacks and Synchronization](#readbacks-and-synchronization)
4. [Queries](#queries)
5. [Dependencies](#dependencies)
6. [Why Wrap the Entire Interface](#why-wrap-the-entire-interface)
7. [Implementation Strategy](#implementation-strategy)
8. [Risks and Edge Cases](#risks-and-edge-cases)
9. [Code Examples](#code-examples)

---

## Core Concepts

### What is ID3D11DeviceContext?

`ID3D11DeviceContext` is the **command queue object** between CPU and GPU. Every rendering operation goes through it:

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
context->Flush();                         // Force GPU to start executing queued commands
```

**Key Point:** GPU and CPU run in parallel. Commands are **queued**, not executed immediately.

### CPU/GPU Parallelism

```
CPU Timeline:
0.1ms: context->Draw(...)        // CPU queues command
0.2ms: context->Map(READ, ...)   // CPU tries to read result

GPU Timeline (parallel):
0.1ms: [still working on previous frame]
5.0ms: [starts processing Draw command]
10ms:  [finishes Draw]

Problem: CPU at 0.2ms tries to read what GPU won't finish until 10ms!
```

---

## Understanding Map Operations

### Map() is NOT Inherently a Read!

`Map()` is a **generic access function** - the `mapType` parameter determines the direction:

```cpp
HRESULT Map(
    ID3D11Resource* resource,
    UINT subresource,
    D3D11_MAP mapType,        // <-- Determines read/write behavior
    UINT mapFlags,
    D3D11_MAPPED_SUBRESOURCE* mappedResource
);
```

### Map Types

#### Write Operations (CPU → GPU) - NO SYNC NEEDED
```cpp
// Most common - completely overwrite resource
D3D11_MAP_WRITE_DISCARD
// Example: Loading UI textures
context->Map(texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
memcpy(mapped.pData, imageData, size);  // CPU writes to GPU memory
context->Unmap(texture);
// GPU can read this whenever - no synchronization needed!

// Write to existing resource
D3D11_MAP_WRITE
// Example: Updating vertex buffer positions
context->Map(vertexBuffer, 0, D3D11_MAP_WRITE, 0, &mapped);
Vertex* verts = (Vertex*)mapped.pData;
verts[0].position = newPos;  // Modify data
context->Unmap(vertexBuffer);

// Append without overwriting
D3D11_MAP_WRITE_NO_OVERWRITE
// Example: Particle system - add new particles to buffer
context->Map(particleBuffer, 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapped);
memcpy(mapped.pData + offset, newParticles, size);
context->Unmap(particleBuffer);
```

#### Read Operations (GPU → CPU) - REQUIRES FLUSH
```cpp
// Read from GPU
D3D11_MAP_READ
// Example: Screenshot/screen capture
context->Draw(...);           // Render scene
context->Flush();             // MUST flush - GPU needs to finish rendering!
context->Map(renderTarget, 0, D3D11_MAP_READ, 0, &mapped);
memcpy(screenshotData, mapped.pData, size);  // CPU reads from GPU
context->Unmap(renderTarget);

// Read and write
D3D11_MAP_READ_WRITE
// Example: Modify existing GPU data based on current values
context->Flush();  // MUST flush first!
context->Map(dataBuffer, 0, D3D11_MAP_READ_WRITE, 0, &mapped);
float* data = (float*)mapped.pData;
data[0] = data[0] * 2.0f;  // Read, modify, write
context->Unmap(dataBuffer);
```

### Menu Lag Root Cause

The 1000 Map operations are almost certainly **WRITE** operations:

```cpp
// What the game probably does (SLOW - 2000ms):
for (int i = 0; i < 1000; i++) {
    context->Map(uiTexture[i], 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    memcpy(mapped.pData, uiData[i], size);  // Load UI element texture
    context->Unmap(uiTexture[i]);
    context->Flush();  // <-- UNNECESSARY! Creates sync point
    // GPU stalls waiting for this texture
}

// What it should do (FAST - 20ms):
for (int i = 0; i < 1000; i++) {
    context->Map(uiTexture[i], 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    memcpy(mapped.pData, uiData[i], size);
    context->Unmap(uiTexture[i]);
    // No flush - batch them all!
}
context->Flush();  // One flush at the end
```

---

## Readbacks and Synchronization

### When Do Readbacks Occur?

Readbacks happen when the **CPU needs GPU results**. These are relatively rare:

#### 1. Screenshots / Video Recording
```cpp
// Render frame
context->Draw(scene);

// Capture to file
context->CopyResource(screenTexture, stagingTexture);
context->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
BYTE* pixels = (BYTE*)mapped.pData;
SaveToPNG(pixels);
context->Unmap(stagingTexture);
```

#### 2. GPU Picking (Click Detection)
```cpp
// User clicks at screen position (x, y)
// Render scene with object IDs as colors
context->ClearRenderTargetView(pickingRT, clearColor);
context->OMSetRenderTargets(1, &pickingRT, depthStencil);

for (auto& object : scene) {
    // Set shader to output object ID as color
    SetObjectIDShader(object.id);
    context->Draw(object);
}

// Read pixel at click position
context->CopyResource(pickingTexture, stagingTexture);
context->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
UINT objectID = ((UINT*)mapped.pData)[y * width + x];
context->Unmap(stagingTexture);

// Now know which object was clicked
printf("Clicked object ID: %d\n", objectID);
```

#### 3. GPU Compute Results
```cpp
// Physics simulation on GPU
context->CSSetShader(physicsShader, ...);
context->Dispatch(numThreads, 1, 1);  // GPU calculates particle positions

// CPU needs results for game logic
context->CopyResource(particleBuffer, stagingBuffer);
context->Map(stagingBuffer, 0, D3D11_MAP_READ, 0, &mapped);
Particle* particles = (Particle*)mapped.pData;

// Update game state based on GPU simulation
UpdateGameLogic(particles);
context->Unmap(stagingBuffer);
```

#### 4. Debug Visualization
```cpp
// Read depth buffer to visualize in tools
context->CopyResource(depthTexture, stagingTexture);
context->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
float* depthData = (float*)mapped.pData;
DrawDebugDepthVisualization(depthData);
context->Unmap(stagingTexture);
```

### Why Flush Before Readback

**Without Flush:**
```cpp
// WRONG - Race condition!
context->Draw(scene);                          // 0.1ms: CPU queues draw
context->Map(renderTarget, 0, D3D11_MAP_READ, ...);  // 0.2ms: CPU tries to read

// GPU timeline:
// 0.1ms: Still working on previous frame
// 5.0ms: Starts processing Draw command
// 10ms: Finishes Draw
// Result: CPU reads STALE DATA from 0.2ms, not the rendered result!
```

**With Flush:**
```cpp
// RIGHT - Synchronized
context->Draw(scene);                          // 0.1ms: CPU queues draw
context->Flush();                              // 0.2ms: Force GPU to execute
                                               // CPU WAITS until GPU finishes
context->Map(renderTarget, 0, D3D11_MAP_READ, ...);  // 10ms: Now safe to read
```

---

## Queries

### What are Queries?

Queries let you **ask the GPU questions** about rendering:

```cpp
// Query types:
D3D11_QUERY_OCCLUSION          // How many pixels were drawn?
D3D11_QUERY_TIMESTAMP          // What time did this happen?
D3D11_QUERY_PIPELINE_STATISTICS // Detailed rendering stats
```

### GetData() - Always Requires Sync

`GetData()` retrieves query results - **always a readback**:

```cpp
HRESULT GetData(
    ID3D11Asynchronous* query,   // The query object
    void* pData,                  // Where to write results (CPU side)
    UINT dataSize,
    UINT getDataFlags
);
```

### Example: Occlusion Query (Visibility Testing)

```cpp
// Create query
ID3D11Query* occlusionQuery;
D3D11_QUERY_DESC queryDesc = { D3D11_QUERY_OCCLUSION, 0 };
device->CreateQuery(&queryDesc, &occlusionQuery);

// Test if object is visible
context->Begin(occlusionQuery);
context->Draw(object);  // Try to draw object
context->End(occlusionQuery);

// Get results - how many pixels were visible?
UINT64 pixelsDrawn;
while (context->GetData(occlusionQuery, &pixelsDrawn, sizeof(pixelsDrawn), 0) == S_FALSE) {
    // GPU still processing - wait
}

if (pixelsDrawn == 0) {
    // Object completely hidden behind walls
    // Skip expensive rendering for this object
    SkipExpensiveEffects(object);
}
```

### Example: Performance Timing

```cpp
// Create timestamp queries
ID3D11Query *startQuery, *endQuery;
D3D11_QUERY_DESC queryDesc = { D3D11_QUERY_TIMESTAMP, 0 };
device->CreateQuery(&queryDesc, &startQuery);
device->CreateQuery(&queryDesc, &endQuery);

// Measure frame time
context->End(startQuery);  // Record start time
context->Draw(scene);      // Do rendering work
context->End(endQuery);    // Record end time

// Get timing results
UINT64 startTime, endTime;
context->GetData(startQuery, &startTime, sizeof(startTime), 0);
context->GetData(endQuery, &endTime, sizeof(endTime), 0);

float frameTimeMs = (endTime - startTime) / 1000000.0f;
printf("Frame took %.2fms\n", frameTimeMs);
```

### Batching Risk with Queries

```cpp
// Game expects immediate results
context->Begin(query);
context->Draw(...);
context->End(query);
context->GetData(query, &result, ...);  // Expects result NOW

// If we buffer the Begin/Draw/End, GetData gets wrong results!
// Solution: Flush before GetData
```

---

## Dependencies

### What are Dependencies?

When one GPU command **depends on the output** of a previous command:

```cpp
// Command 1: Render scene to texture A
context->OMSetRenderTargets(1, &textureA, ...);
context->Draw(scene);  // Writes to texture A

// Command 2: Use texture A as input for post-processing
context->PSSetShaderResources(0, 1, &textureA);  // Reads from texture A
context->Draw(fullscreenQuad);  // This NEEDS command 1 to finish first!
```

### Dependency Types

#### Resource Dependencies (Read-After-Write)
```cpp
// Write to texture
context->Draw(...);  // Renders to renderTarget

// Read from same texture
context->PSSetShaderResources(0, 1, &renderTarget);
context->Draw(...);  // Uses renderTarget as input

// Dependency: Second draw needs first draw's output
```

#### State Dependencies
```cpp
// Set render target
context->OMSetRenderTargets(1, &targetA, ...);
context->Draw(...);  // Draws to targetA

// Change render target
context->OMSetRenderTargets(1, &targetB, ...);
context->Draw(...);  // Draws to targetB

// Dependency: Render target changes must happen in order
```

#### Timing Dependencies
```cpp
context->Begin(query);
context->Draw(...);  // Commands between Begin/End are measured
context->End(query);

// Dependency: Begin/End must bracket the measured work
```

### Batching Risks

```cpp
// DANGEROUS batching - breaks dependencies!
Batch 1: {
    Draw to texture A,    // Creates texture A
    Draw using texture B  // Unrelated
}

Batch 2: {
    Draw using texture A, // Needs texture A from Batch 1
    Draw to texture C     // Unrelated
}

// Problem: Batch 2 starts before Batch 1 finishes!
// Second draw uses STALE texture A data
```

### Safe Batching Strategy

```cpp
// Option 1: Conservative - Flush on resource transitions
if (resourceUsedAsInputWasPreviouslyOutput) {
    FlushBatch();  // Ensure previous writes complete
}

// Option 2: Dependency tracking (complex but optimal)
struct Command {
    CommandType type;
    std::set<ResourceID> reads;   // Resources this command reads
    std::set<ResourceID> writes;  // Resources this command writes
};

void AddCommand(Command cmd) {
    // Check if any buffered command writes what this command reads
    for (auto& buffered : commandBuffer) {
        if (HasDependency(buffered, cmd)) {
            FlushBatch();  // Resolve dependency
            break;
        }
    }
    commandBuffer.push_back(cmd);
}
```

---

## Why Wrap the Entire Interface

### The ID3D11DeviceContext Interface

ID3D11DeviceContext has **~150 virtual methods**:

```cpp
class ID3D11DeviceContext : public ID3D11DeviceChild {
public:
    // Drawing
    virtual void Draw(UINT vertexCount, UINT startVertex) = 0;
    virtual void DrawIndexed(UINT indexCount, UINT startIndex, INT baseVertex) = 0;
    virtual void DrawInstanced(...) = 0;
    virtual void DrawIndexedInstanced(...) = 0;
    // ... 15+ more draw variants
    
    // Resource access
    virtual HRESULT Map(ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*) = 0;
    virtual void Unmap(ID3D11Resource*, UINT) = 0;
    virtual void UpdateSubresource(...) = 0;
    virtual void CopyResource(ID3D11Resource* dst, ID3D11Resource* src) = 0;
    virtual void CopySubresourceRegion(...) = 0;
    // ... 10+ more resource methods
    
    // State setting
    virtual void PSSetShader(ID3D11PixelShader*, ID3D11ClassInstance**, UINT) = 0;
    virtual void VSSetShader(ID3D11VertexShader*, ID3D11ClassInstance**, UINT) = 0;
    virtual void CSSetShader(ID3D11ComputeShader*, ID3D11ClassInstance**, UINT) = 0;
    virtual void IASetVertexBuffers(UINT, UINT, ID3D11Buffer**, const UINT*, const UINT*) = 0;
    virtual void IASetIndexBuffer(ID3D11Buffer*, DXGI_FORMAT, UINT) = 0;
    virtual void OMSetRenderTargets(UINT, ID3D11RenderTargetView**, ID3D11DepthStencilView*) = 0;
    // ... 100+ more state methods
    
    // Queries and sync
    virtual void Begin(ID3D11Asynchronous*) = 0;
    virtual void End(ID3D11Asynchronous*) = 0;
    virtual HRESULT GetData(ID3D11Asynchronous*, void*, UINT, UINT) = 0;
    virtual void Flush() = 0;
    
    // ... many more methods
};
```

### Why You Must Wrap All Methods

If you don't wrap a method, the game will call the **real context** directly, bypassing your batching:

```cpp
// Your wrapper only implements Draw()
class PartialWrapper : public ID3D11DeviceContext {
    virtual void Draw(UINT count, UINT start) override {
        Buffer(CMD_DRAW, count, start);  // Your batching logic
    }
    
    // DrawIndexed NOT wrapped - compiler error or crashes!
};

// Game calls DrawIndexed:
context->DrawIndexed(300, 0, 0);
// Either: Compiler error (pure virtual not implemented)
// Or: Calls wrong implementation, crashes
```

### The Wrapper Pattern

```cpp
class WrappedContext : public ID3D11DeviceContext {
private:
    ID3D11DeviceContext* realContext;  // The actual DXVK/system context
    std::vector<Command> commandBuffer;
    
public:
    WrappedContext(ID3D11DeviceContext* real) : realContext(real) {}
    
    // Wrap methods you care about (batching logic)
    virtual void Draw(UINT count, UINT start) override {
        commandBuffer.push_back({CMD_DRAW, count, start});
        
        if (commandBuffer.size() >= 1000) {
            FlushBatch();
        }
    }
    
    virtual HRESULT Map(ID3D11Resource* res, UINT subres,
                        D3D11_MAP mapType, UINT flags,
                        D3D11_MAPPED_SUBRESOURCE* mapped) override {
        // Check if this is a readback
        if (mapType == D3D11_MAP_READ || mapType == D3D11_MAP_READ_WRITE) {
            Log("Readback detected - flushing batch");
            FlushBatch();
            realContext->Flush();
        }
        
        // Forward to real implementation
        return realContext->Map(res, subres, mapType, flags, mapped);
    }
    
    virtual void Flush() override {
        Log("Flush called");
        // Only flush if really necessary (not on every call)
        if (ShouldReallyFlush()) {
            FlushBatch();
            realContext->Flush();
        }
    }
    
    // Forward everything else (simple pass-through)
    virtual void DrawIndexed(UINT indexCount, UINT startIndex, INT baseVertex) override {
        realContext->DrawIndexed(indexCount, startIndex, baseVertex);
    }
    
    virtual void PSSetShader(ID3D11PixelShader* shader, 
                            ID3D11ClassInstance** instances, 
                            UINT numInstances) override {
        realContext->PSSetShader(shader, instances, numInstances);
    }
    
    virtual void CopyResource(ID3D11Resource* dst, ID3D11Resource* src) override {
        realContext->CopyResource(dst, src);
    }
    
    // ... 147 more forwarding methods ...
    
private:
    void FlushBatch() {
        for (auto& cmd : commandBuffer) {
            if (cmd.type == CMD_DRAW) {
                realContext->Draw(cmd.count, cmd.start);
            }
        }
        commandBuffer.clear();
    }
    
    bool ShouldReallyFlush() {
        // Heuristics: flush before present, queries, etc.
        return true;  // Simplified
    }
};
```

### Methods That Need Special Handling

Most methods can be simple forwarders, but these need batching logic:

```cpp
// Critical for batching:
Map() / Unmap()           // Check for readbacks
Flush()                   // Intercept unnecessary flushes
UpdateSubresource()       // Buffer resource updates

// Queries - force flush:
Begin() / End()           // Query boundaries
GetData()                 // Reading query results

// State that affects batching:
OMSetRenderTargets()      // Render target changes
PSSetShaderResources()    // Resource binding (check dependencies)

// All others:
Draw*, Clear*, Set*       // Simple forward to realContext
```

---

## Implementation Strategy

### Phase 1: Minimal Wrapper (1-2 days)

Get the DLL loading and basic interception working:

```cpp
// d3d11_minimal.cpp
#include <d3d11.h>
#include <fstream>

// Forward declarations
ID3D11DeviceContext* g_realContext = nullptr;

void Log(const char* msg) {
    std::ofstream log("d3d11_log.txt", std::ios::app);
    log << msg << std::endl;
}

// Minimal wrapper - just logs and forwards
class MinimalWrapper : public ID3D11DeviceContext {
private:
    ID3D11DeviceContext* real;
    
public:
    MinimalWrapper(ID3D11DeviceContext* r) : real(r) {
        Log("MinimalWrapper created");
    }
    
    // Implement ALL methods as simple forwards
    virtual void Draw(UINT count, UINT start) override {
        Log("Draw called");
        real->Draw(count, start);
    }
    
    virtual void Map(ID3D11Resource* res, UINT subres, D3D11_MAP type,
                    UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped) override {
        char buf[256];
        sprintf(buf, "Map called - type: %d", type);
        Log(buf);
        return real->Map(res, subres, type, flags, mapped);
    }
    
    // ... 148 more simple forwards ...
};

// Hook D3D11CreateDevice
extern "C" __declspec(dllexport) 
HRESULT WINAPI D3D11CreateDevice(..., ID3D11DeviceContext** ppContext) {
    Log("D3D11CreateDevice intercepted");
    
    // Call real function
    HRESULT hr = RealD3D11CreateDevice(..., &g_realContext);
    
    // Return wrapped context
    *ppContext = new MinimalWrapper(g_realContext);
    
    return hr;
}
```

**Goal:** Verify DLL loads, game runs, logs show interception

### Phase 2: Map/Flush Optimization (1 week)

Focus on the menu lag issue:

```cpp
class MenuOptimizedWrapper : public ID3D11DeviceContext {
private:
    ID3D11DeviceContext* real;
    int flushCount = 0;
    int mapWriteCount = 0;
    
public:
    virtual HRESULT Map(ID3D11Resource* res, UINT subres, D3D11_MAP type,
                       UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped) override {
        
        // Detect write operations
        if (type == D3D11_MAP_WRITE_DISCARD || type == D3D11_MAP_WRITE) {
            mapWriteCount++;
        }
        
        // Only flush if reading
        if (type == D3D11_MAP_READ || type == D3D11_MAP_READ_WRITE) {
            char buf[256];
            sprintf(buf, "READBACK detected at Map #%d - forcing flush", mapWriteCount);
            Log(buf);
            real->Flush();
        }
        
        return real->Map(res, subres, type, flags, mapped);
    }
    
    virtual void Flush() override {
        flushCount++;
        
        // Heuristic: Ignore most flush calls
        // Only flush every Nth call or before present
        if (flushCount % 100 == 0) {
            char buf[256];
            sprintf(buf, "Allowing flush #%d", flushCount);
            Log(buf);
            real->Flush();
        } else {
            char buf[256];
            sprintf(buf, "Ignoring flush #%d", flushCount);
            Log(buf);
            // Don't flush!
        }
    }
    
    // ... rest of methods forward normally ...
};
```

**Goal:** Reduce menu lag by removing unnecessary flushes

### Phase 3: Command Batching (2-4 weeks)

Implement full batching system:

```cpp
struct Command {
    enum Type { DRAW, DRAW_INDEXED, MAP, UNMAP, UPDATE_SUBRESOURCE };
    Type type;
    
    // Union of all possible parameters
    union {
        struct { UINT vertexCount, startVertex; } draw;
        struct { ID3D11Resource* resource; UINT subresource; } unmap;
        // ... etc
    } params;
};

class BatchedWrapper : public ID3D11DeviceContext {
private:
    ID3D11DeviceContext* real;
    std::vector<Command> commandBuffer;
    const int BATCH_SIZE = 1000;
    
public:
    virtual void Draw(UINT count, UINT start) override {
        Command cmd;
        cmd.type = Command::DRAW;
        cmd.params.draw = {count, start};
        
        commandBuffer.push_back(cmd);
        
        if (commandBuffer.size() >= BATCH_SIZE) {
            FlushBatch();
        }
    }
    
    virtual HRESULT Map(ID3D11Resource* res, UINT subres, D3D11_MAP type,
                       UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped) override {
        
        // Readback - must flush first
        if (type == D3D11_MAP_READ || type == D3D11_MAP_READ_WRITE) {
            FlushBatch();
            real->Flush();
        }
        
        // Write operations can be deferred
        // (But Map itself returns immediately, just defer the Unmap/update)
        
        return real->Map(res, subres, type, flags, mapped);
    }
    
    virtual void Flush() override {
        // Intercept - only flush when necessary
        // (Before present, queries, etc.)
        FlushBatch();
        real->Flush();
    }
    
private:
    void FlushBatch() {
        Log("Flushing batch of %d commands", commandBuffer.size());
        
        for (auto& cmd : commandBuffer) {
            switch (cmd.type) {
                case Command::DRAW:
                    real->Draw(cmd.params.draw.vertexCount, 
                             cmd.params.draw.startVertex);
                    break;
                case Command::UNMAP:
                    real->Unmap(cmd.params.unmap.resource,
                              cmd.params.unmap.subresource);
                    break;
                // ... other command types
            }
        }
        
        commandBuffer.clear();
    }
};
```

**Goal:** Full command batching with dependency tracking

### Phase 4: Polish and Testing (1-2 weeks)

- Test across all three Arland games
- Handle edge cases
- Optimize batch size
- Add configuration file
- Remove debug logging for release

---

## Risks and Edge Cases

### 1. Readback Detection

**Risk:** Missing a readback operation leads to reading stale data

```cpp
// Potential bug:
context->Draw(scene);
// We buffer this ^

context->Map(renderTarget, D3D11_MAP_READ, ...);  
// Reads OLD data because Draw not executed yet!

// Solution: Always flush before READ maps
if (mapType == D3D11_MAP_READ || mapType == D3D11_MAP_READ_WRITE) {
    FlushBatch();
    realContext->Flush();  // Ensure GPU finishes
}
```

### 2. Query Timing

**Risk:** Buffering Begin/End breaks query measurements

```cpp
// Game code:
context->Begin(query);
context->Draw(object);
context->End(query);
context->GetData(query, &result, ...);

// If we buffer Begin/Draw/End:
commandBuffer = [Begin, Draw, End];  // Not executed yet!
// GetData returns wrong/invalid results

// Solution: Flush before GetData
virtual HRESULT GetData(ID3D11Asynchronous* query, ...) override {
    FlushBatch();
    return realContext->GetData(query, ...);
}
```

### 3. Resource Lifetime

**Risk:** Resource deleted while commands still buffered

```cpp
// Game code:
context->Draw(mesh);           // Uses vertexBuffer
vertexBuffer->Release();        // Buffer deleted!

// Our buffer:
commandBuffer = [Draw using vertexBuffer];  // Dangles!

// When we flush:
realContext->Draw(...);         // Crash - buffer is gone!

// Solution: AddRef resources in buffered commands
void AddCommand(Command cmd) {
    if (cmd.hasResource) {
        cmd.resource->AddRef();  // Keep resource alive
    }
    commandBuffer.push_back(cmd);
}

void FlushBatch() {
    for (auto& cmd : commandBuffer) {
        ExecuteCommand(cmd);
        if (cmd.hasResource) {
            cmd.resource->Release();  // Now safe to release
        }
    }
}
```

### 4. Render Target Transitions

**Risk:** Using texture as input before it's finished being rendered to

```cpp
// Render to texture
context->OMSetRenderTargets(1, &textureAsRT, ...);
context->Draw(scene);  // Writes to texture

// Use as shader input
context->PSSetShaderResources(0, 1, &textureAsSRV);
context->Draw(quad);   // Reads from texture

// If buffered incorrectly:
Batch 1: [Draw to texture A, Draw using texture B]
Batch 2: [Draw using texture A, ...]  // Reads before write completes!

// Solution: Flush on render target → shader resource transition
void PSSetShaderResources(UINT slot, UINT count, ID3D11ShaderResourceView** views) {
    for (UINT i = 0; i < count; i++) {
        if (WasRecentlyRenderTarget(views[i])) {
            FlushBatch();  // Ensure render completes
            break;
        }
    }
    realContext->PSSetShaderResources(slot, count, views);
}
```

### 5. Present() Timing

**Risk:** Buffered commands not executing before frame presentation

```cpp
// Game renders frame
context->Draw(scene);

// Present to screen
swapChain->Present(0, 0);  // Show frame NOW

// If commands buffered:
// Screen shows OLD frame, new frame still in buffer!

// Solution: Flush before Present
// (Usually handled by DXGI, but worth intercepting IDXGISwapChain::Present too)
```

### 6. Multi-threading

**Risk:** Multiple threads calling context methods

```cpp
// Thread 1:
context->Draw(mesh1);

// Thread 2:
context->Draw(mesh2);  // Race condition on commandBuffer!

// Solution: D3D11 immediate context is single-threaded by design
// If game uses deferred contexts, handle separately
// Add mutex if needed:
std::mutex commandBufferMutex;

void Draw(UINT count, UINT start) {
    std::lock_guard<std::mutex> lock(commandBufferMutex);
    commandBuffer.push_back({CMD_DRAW, count, start});
}
```

### 7. State Leakage

**Risk:** Buffering state changes causes rendering with wrong state

```cpp
// Set shader A
context->PSSetShader(shaderA, ...);
context->Draw(mesh1);  // Should use shader A

// Set shader B
context->PSSetShader(shaderB, ...);
context->Draw(mesh2);  // Should use shader B

// If we buffer and execute together:
commandBuffer = [SetShader A, Draw, SetShader B, Draw];
// When executed in batch, both draws might use shader B!

// Solution: Don't buffer state changes, apply immediately
virtual void PSSetShader(...) override {
    realContext->PSSetShader(...);  // Don't buffer
}
```

### 8. Nested Map Calls

**Risk:** Mapping same resource multiple times

```cpp
context->Map(buffer, D3D11_MAP_WRITE, ...);    // Lock buffer
context->Map(buffer, D3D11_MAP_WRITE, ...);    // ERROR: Already locked!

// D3D11 doesn't allow this - track locked resources
std::set<ID3D11Resource*> lockedResources;

virtual HRESULT Map(ID3D11Resource* res, ...) override {
    if (lockedResources.count(res)) {
        Log("ERROR: Resource already mapped!");
        return E_FAIL;
    }
    lockedResources.insert(res);
    return realContext->Map(res, ...);
}

virtual void Unmap(ID3D11Resource* res, ...) override {
    lockedResources.erase(res);
    realContext->Unmap(res, ...);
}
```

---

## Code Examples

### Complete Minimal Working Example

```cpp
// d3d11_interposer.cpp
#include <windows.h>
#include <d3d11.h>
#include <fstream>
#include <vector>

// Logging
void Log(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    std::ofstream log("d3d11_interposer.txt", std::ios::app);
    log << buffer << std::endl;
}

// Load real D3D11
typedef HRESULT (WINAPI *PFN_D3D11CreateDevice)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT,
    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

PFN_D3D11CreateDevice RealD3D11CreateDevice = nullptr;
HMODULE hRealD3D11 = nullptr;

// Wrapper class
class InterceptedContext : public ID3D11DeviceContext {
private:
    ID3D11DeviceContext* m_real;
    ULONG m_refCount;
    
public:
    InterceptedContext(ID3D11DeviceContext* real) 
        : m_real(real), m_refCount(1) {
        Log("InterceptedContext created");
    }
    
    // IUnknown
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        return m_real->QueryInterface(riid, ppvObject);
    }
    
    virtual ULONG STDMETHODCALLTYPE AddRef() override {
        return ++m_refCount;
    }
    
    virtual ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = --m_refCount;
        if (count == 0) {
            m_real->Release();
            delete this;
        }
        return count;
    }
    
    // ID3D11DeviceChild
    virtual void STDMETHODCALLTYPE GetDevice(ID3D11Device** ppDevice) override {
        m_real->GetDevice(ppDevice);
    }
    
    virtual HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override {
        return m_real->GetPrivateData(guid, pDataSize, pData);
    }
    
    virtual HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override {
        return m_real->SetPrivateData(guid, DataSize, pData);
    }
    
    virtual HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override {
        return m_real->SetPrivateDataInterface(guid, pData);
    }
    
    // ID3D11DeviceContext - Key methods
    virtual void STDMETHODCALLTYPE Map(
        ID3D11Resource* pResource,
        UINT Subresource,
        D3D11_MAP MapType,
        UINT MapFlags,
        D3D11_MAPPED_SUBRESOURCE* pMappedResource) override {
        
        // Log map type
        const char* mapTypeStr[] = {"READ", "WRITE", "READ_WRITE", "WRITE_DISCARD", "WRITE_NO_OVERWRITE"};
        Log("Map called - Type: %s", mapTypeStr[MapType - 1]);
        
        // Check for readback
        if (MapType == D3D11_MAP_READ || MapType == D3D11_MAP_READ_WRITE) {
            Log("READBACK detected - would flush batch here");
            m_real->Flush();
        }
        
        return m_real->Map(pResource, Subresource, MapType, MapFlags, pMappedResource);
    }
    
    virtual void STDMETHODCALLTYPE Flush() override {
        static int flushCount = 0;
        Log("Flush #%d called", ++flushCount);
        m_real->Flush();
    }
    
    virtual void STDMETHODCALLTYPE Draw(UINT VertexCount, UINT StartVertexLocation) override {
        Log("Draw: %d vertices from %d", VertexCount, StartVertexLocation);
        m_real->Draw(VertexCount, StartVertexLocation);
    }
    
    // Forward all other methods (150+ methods - use macro or code generator)
    virtual void STDMETHODCALLTYPE DrawIndexed(UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation) override {
        m_real->DrawIndexed(IndexCount, StartIndexLocation, BaseVertexLocation);
    }
    
    virtual void STDMETHODCALLTYPE Unmap(ID3D11Resource* pResource, UINT Subresource) override {
        m_real->Unmap(pResource, Subresource);
    }
    
    // ... 147 more method forwards ...
    // (In practice, use a script or macro to generate these)
};

// DLL entry point
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        Log("=== D3D11 Interposer Loaded ===");
        
        // Load real d3d11.dll
        char sysPath[MAX_PATH];
        GetSystemDirectoryA(sysPath, MAX_PATH);
        strcat_s(sysPath, "\\d3d11.dll");
        
        hRealD3D11 = LoadLibraryA(sysPath);
        if (hRealD3D11) {
            RealD3D11CreateDevice = (PFN_D3D11CreateDevice)
                GetProcAddress(hRealD3D11, "D3D11CreateDevice");
            Log("Real d3d11.dll loaded successfully");
        } else {
            Log("ERROR: Failed to load real d3d11.dll");
        }
    }
    return TRUE;
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
    Log("D3D11CreateDevice intercepted!");
    
    if (!RealD3D11CreateDevice) {
        Log("ERROR: Real D3D11CreateDevice not loaded");
        return E_FAIL;
    }
    
    // Call real function
    ID3D11DeviceContext* realContext = nullptr;
    HRESULT hr = RealD3D11CreateDevice(
        pAdapter, DriverType, Software, Flags,
        pFeatureLevels, FeatureLevels, SDKVersion,
        ppDevice, pFeatureLevel, &realContext);
    
    if (SUCCEEDED(hr) && ppImmediateContext) {
        // Return wrapped context
        *ppImmediateContext = new InterceptedContext(realContext);
        Log("Context wrapped successfully");
    }
    
    return hr;
}
```

### Build Script

```bash
#!/bin/bash
# build.sh

GAME_DIR="/home/username/.steam/debian-installation/steamapps/common/Atelier Meruru ~The Apprentice of Arland~ DX/"

echo "Compiling d3d11_interposer.cpp to Windows DLL..."
x86_64-w64-mingw32-g++ -shared -o d3d11.dll d3d11_interposer.cpp \
    -static-libgcc -static-libstdc++ \
    -ld3d11 -ldxgi -luuid \
    -O2 -Wall

if [ $? -ne 0 ]; then
    echo "Compilation failed!"
    exit 1
fi

echo "Compilation successful!"
echo "Copying to game directory..."
cp d3d11.dll "$GAME_DIR"

echo "Done! d3d11.dll deployed"
echo "Launch game with: WINEDLLOVERRIDES=\"d3d11=n,b\" %command%"
```

### Testing Checklist

```
[ ] DLL loads without crashing game
[ ] Log file created with "D3D11CreateDevice intercepted"
[ ] Game renders correctly
[ ] Map operations logged correctly
[ ] Flush operations logged correctly
[ ] Menu opens without crash
[ ] Menu lag reduced (measure with DXVK_HUD)
[ ] No graphical glitches
[ ] Game exits cleanly
[ ] Works across different Atelier games
```

---

## Performance Expectations

### Current (Broken)
```
Menu open: 2000ms
- 1000 Map(WRITE) operations
- 1000 Flush operations
- Each Map+Flush = ~2ms of sync overhead
```

### After Basic Flush Removal
```
Menu open: 500ms (4x improvement)
- 1000 Map(WRITE) operations
- Flush calls ignored/deferred
- Batch flush at end
```

### After Full Batching
```
Menu open: 20-100ms (20-100x improvement)
- Commands buffered and executed in batches
- Minimal sync points
- GPU pipeline stays full
```

---

## Debugging Tips

### Enable Verbose Logging

```cpp
#define DEBUG_LOGGING 1

void Log(const char* format, ...) {
#if DEBUG_LOGGING
    // ... log to file
#endif
}
```

### Track Statistics

```cpp
struct Stats {
    int mapWriteCount = 0;
    int mapReadCount = 0;
    int flushCount = 0;
    int flushIgnoredCount = 0;
    int drawCount = 0;
};

Stats g_stats;

// In Flush():
if (ShouldIgnoreFlush()) {
    g_stats.flushIgnoredCount++;
} else {
    g_stats.flushCount++;
}

// Dump on exit:
void DumpStats() {
    Log("=== Statistics ===");
    Log("Map(WRITE): %d", g_stats.mapWriteCount);
    Log("Map(READ): %d", g_stats.mapReadCount);
    Log("Flushes: %d", g_stats.flushCount);
    Log("Flushes ignored: %d", g_stats.flushIgnoredCount);
}
```

### DXVK HUD Monitoring

```bash
# Before interposer:
DXVK_HUD=full WINEDLLOVERRIDES="d3d11=b" %command%
# Note: Queue submissions, syncs

# After interposer:
DXVK_HUD=full WINEDLLOVERRIDES="d3d11=n,b" %command%
# Compare: Should see fewer submissions/syncs
```

---

## Next Steps

1. **Implement complete method forwarding** (use code generator)
2. **Test basic interception** (logging only, no batching)
3. **Add Flush() filtering** (ignore unnecessary flushes)
4. **Implement command buffering** (buffer Draw, Map, etc.)
5. **Add readback detection** (flush before MAP_READ)
6. **Test across all three Arland games**
7. **Optimize and tune batch sizes**
8. **Add configuration file**
9. **Remove debug code for release**
10. **Document and publish**

---

## References

- **Microsoft D3D11 Documentation**: https://docs.microsoft.com/en-us/windows/win32/api/d3d11/
- **DXVK Source Code**: https://github.com/doitsujin/dxvk
- **atelier-sync-fix**: https://github.com/doitsujin/atelier-sync-fix (inspiration)

---

**Good luck with your interposer! This is a challenging but rewarding project.**