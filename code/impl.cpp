#include <atomic>
#include <chrono>
#include <iomanip>
#include <sstream>

#include "impl.h"
#include "util.h"

namespace atfix {

/** Hooking-related stuff */
using PFN_ID3D11DeviceContext_Map = HRESULT (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using PFN_ID3D11DeviceContext_Unmap = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11Resource*, UINT);
using PFN_ID3D11DeviceContext_Flush = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*);
using PFN_ID3D11DeviceContext_Draw = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  UINT, UINT);
using PFN_ID3D11DeviceContext_DrawIndexed = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  UINT, UINT, INT);
using PFN_ID3D11DeviceContext_DrawInstanced = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  UINT, UINT, UINT, UINT);
using PFN_ID3D11DeviceContext_DrawIndexedInstanced = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  UINT, UINT, UINT, INT, UINT);

struct ContextProcs {
  PFN_ID3D11DeviceContext_Map                   Map                   = nullptr;
  PFN_ID3D11DeviceContext_Unmap                 Unmap                 = nullptr;
  PFN_ID3D11DeviceContext_Flush                 Flush                 = nullptr;
  PFN_ID3D11DeviceContext_Draw                  Draw                  = nullptr;
  PFN_ID3D11DeviceContext_DrawIndexed           DrawIndexed           = nullptr;
  PFN_ID3D11DeviceContext_DrawInstanced         DrawInstanced         = nullptr;
  PFN_ID3D11DeviceContext_DrawIndexedInstanced  DrawIndexedInstanced  = nullptr;
};

static mutex  g_hookMutex;

ContextProcs  g_immContextProcs;
ContextProcs  g_defContextProcs;

constexpr uint32_t HOOK_IMM_CTX = (1u << 0);
constexpr uint32_t HOOK_DEF_CTX = (1u << 1);

uint32_t      g_installedHooks = 0u;

// Config
static bool g_enableLogging = false;  // Set to true to enable verbose logging

// Counters
static std::atomic<uint64_t> g_drawCount = 0;
static std::atomic<uint64_t> g_mapCount = 0;
static std::atomic<uint64_t> g_unmapCount = 0;
static std::atomic<uint64_t> g_flushCount = 0;

// Timing
static auto g_startTime = std::chrono::high_resolution_clock::now();

const ContextProcs* getContextProcs(ID3D11DeviceContext* pContext) {
  return pContext->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE
    ? &g_immContextProcs
    : &g_defContextProcs;
}

bool isImmediateContext(ID3D11DeviceContext* pContext) {
  return pContext->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
}

std::string getTimestamp() {
  auto now = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(now - g_startTime);
  double seconds = duration.count() / 1000000.0;

  std::ostringstream oss;
  oss << std::fixed << std::setprecision(6) << seconds;
  return oss.str();
}

const char* mapTypeToString(D3D11_MAP MapType) {
  switch (MapType) {
    case D3D11_MAP_READ:                return "READ";
    case D3D11_MAP_WRITE:               return "WRITE";
    case D3D11_MAP_READ_WRITE:          return "READ_WRITE";
    case D3D11_MAP_WRITE_DISCARD:       return "WRITE_DISCARD";
    case D3D11_MAP_WRITE_NO_OVERWRITE:  return "WRITE_NO_OVERWRITE";
    default:                            return "UNKNOWN";
  }
}

const char* resourceDimToString(D3D11_RESOURCE_DIMENSION Dim) {
  switch (Dim) {
    case D3D11_RESOURCE_DIMENSION_BUFFER:     return "Buffer";
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D:  return "Tex1D";
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D:  return "Tex2D";
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D:  return "Tex3D";
    default:                                  return "Unknown";
  }
}

/** Hooked functions */

HRESULT STDMETHODCALLTYPE ID3D11DeviceContext_Map(
        ID3D11DeviceContext*      pContext,
        ID3D11Resource*           pResource,
        UINT                      Subresource,
        D3D11_MAP                 MapType,
        UINT                      MapFlags,
        D3D11_MAPPED_SUBRESOURCE* pMappedResource) {
  auto procs = getContextProcs(pContext);

  if (g_enableLogging) {
    uint64_t mapNum = ++g_mapCount;
    uint64_t drawNum = g_drawCount;

    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    if (pResource)
      pResource->GetType(&dim);

    log("[", getTimestamp(), "s] [Draw#", drawNum, "] Map #", mapNum,
        " - ", mapTypeToString(MapType), " ", resourceDimToString(dim));
  }

  return procs->Map(pContext, pResource, Subresource, MapType, MapFlags, pMappedResource);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_Unmap(
        ID3D11DeviceContext*      pContext,
        ID3D11Resource*           pResource,
        UINT                      Subresource) {
  auto procs = getContextProcs(pContext);

  if (g_enableLogging) {
    uint64_t unmapNum = ++g_unmapCount;
    uint64_t drawNum = g_drawCount;
    log("[", getTimestamp(), "s] [Draw#", drawNum, "] Unmap #", unmapNum);
  }

  procs->Unmap(pContext, pResource, Subresource);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_Flush(
        ID3D11DeviceContext*      pContext) {
  auto procs = getContextProcs(pContext);

  // FLUSH FILTERING: Ignore ALL flush calls
  if (g_enableLogging) {
    uint64_t flushNum = ++g_flushCount;
    uint64_t drawNum = g_drawCount;
    log("[", getTimestamp(), "s] [Draw#", drawNum, "] Flush #", flushNum, " IGNORED");
  }

  return;  // Don't actually flush
}

void STDMETHODCALLTYPE ID3D11DeviceContext_Draw(
        ID3D11DeviceContext*      pContext,
        UINT                      VertexCount,
        UINT                      StartVertexLocation) {
  auto procs = getContextProcs(pContext);
  g_drawCount++;
  procs->Draw(pContext, VertexCount, StartVertexLocation);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_DrawIndexed(
        ID3D11DeviceContext*      pContext,
        UINT                      IndexCount,
        UINT                      StartIndexLocation,
        INT                       BaseVertexLocation) {
  auto procs = getContextProcs(pContext);
  g_drawCount++;
  procs->DrawIndexed(pContext, IndexCount, StartIndexLocation, BaseVertexLocation);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_DrawInstanced(
        ID3D11DeviceContext*      pContext,
        UINT                      VertexCountPerInstance,
        UINT                      InstanceCount,
        UINT                      StartVertexLocation,
        UINT                      StartInstanceLocation) {
  auto procs = getContextProcs(pContext);
  g_drawCount++;
  procs->DrawInstanced(pContext, VertexCountPerInstance, InstanceCount,
                       StartVertexLocation, StartInstanceLocation);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_DrawIndexedInstanced(
        ID3D11DeviceContext*      pContext,
        UINT                      IndexCountPerInstance,
        UINT                      InstanceCount,
        UINT                      StartIndexLocation,
        INT                       BaseVertexLocation,
        UINT                      StartInstanceLocation) {
  auto procs = getContextProcs(pContext);
  g_drawCount++;
  procs->DrawIndexedInstanced(pContext, IndexCountPerInstance, InstanceCount,
                              StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
}

#define HOOK_PROC(iface, object, table, index, proc) \
  hookProc(object, #iface "::" #proc, &table->proc, &iface ## _ ## proc, index)

template<typename T>
void hookProc(void* pObject, const char* pName, T** ppOrig, T* pHook, uint32_t index) {
  void** vtbl = *reinterpret_cast<void***>(pObject);

  MH_STATUS mh = MH_CreateHook(vtbl[index],
    reinterpret_cast<void*>(pHook),
    reinterpret_cast<void**>(ppOrig));

  if (mh) {
    if (mh != MH_ERROR_ALREADY_CREATED)
      log("Failed to create hook for ", pName, ": ", MH_StatusToString(mh));
    return;
  }

  mh = MH_EnableHook(vtbl[index]);

  if (mh) {
    log("Failed to enable hook for ", pName, ": ", MH_StatusToString(mh));
    return;
  }

  log("Created hook for ", pName, " @ ", reinterpret_cast<void*>(pHook));
}

void hookDevice(ID3D11Device* pDevice) {
  // No device hooks needed for minimal logging
  log("Device created: ", pDevice);
}

void hookContext(ID3D11DeviceContext* pContext) {
  std::lock_guard lock(g_hookMutex);

  uint32_t flag = HOOK_IMM_CTX;
  ContextProcs* procs = &g_immContextProcs;

  if (!isImmediateContext(pContext)) {
    flag = HOOK_DEF_CTX;
    procs = &g_defContextProcs;
  }

  if (g_installedHooks & flag)
    return;

  log("Hooking context ", pContext);

  // Map/Unmap/Flush hooks
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 14, Map);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 15, Unmap);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 27, Flush);

  // Draw counting hooks
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 13, Draw);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 12, DrawIndexed);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 20, DrawInstanced);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 21, DrawIndexedInstanced);

  g_installedHooks |= flag;

  /* Immediate context and deferred context methods may share code */
  if (flag & HOOK_IMM_CTX)
    g_defContextProcs = g_immContextProcs;
}

}
