#include <array>
#include <cstring>
#include <iomanip>
#include <sstream>

#include "impl.h"
#include "trace.h"
#include "util.h"

namespace atfix {

/** Hooking-related stuff */
using PFN_ID3D11DeviceContext_Map = HRESULT (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using PFN_ID3D11DeviceContext_Unmap = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11Resource*, UINT);
using PFN_ID3D11DeviceContext_CopyResource = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11Resource*, ID3D11Resource*);
using PFN_ID3D11DeviceContext_CopySubresourceRegion = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);

struct ContextProcs {
  PFN_ID3D11DeviceContext_Map                   Map                   = nullptr;
  PFN_ID3D11DeviceContext_Unmap                 Unmap                 = nullptr;
  PFN_ID3D11DeviceContext_CopyResource          CopyResource          = nullptr;
  PFN_ID3D11DeviceContext_CopySubresourceRegion CopySubresourceRegion = nullptr;
};

static mutex  g_hookMutex;

ContextProcs  g_immContextProcs;
ContextProcs  g_defContextProcs;

constexpr uint32_t HOOK_IMM_CTX = (1u << 0);
constexpr uint32_t HOOK_DEF_CTX = (1u << 1);

uint32_t      g_installedHooks = 0u;

const ContextProcs* getContextProcs(ID3D11DeviceContext* pContext) {
  return pContext->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE
    ? &g_immContextProcs
    : &g_defContextProcs;
}

bool isImmediateContext(ID3D11DeviceContext* pContext) {
  return pContext->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
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

  // Call real Map first
  HRESULT hr = procs->Map(pContext, pResource, Subresource, MapType, MapFlags, pMappedResource);

  // Log Map operations on Tex2D (if logging active and Map succeeded)
  if (SUCCEEDED(hr) && isTraceLoggingActive() && pResource && pMappedResource) {
    D3D11_RESOURCE_DIMENSION dim;
    pResource->GetType(&dim);

    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
      ID3D11Texture2D* tex = nullptr;
      pResource->QueryInterface(IID_PPV_ARGS(&tex));

      D3D11_TEXTURE2D_DESC desc = {};
      tex->GetDesc(&desc);

      // Log Map(READ) on STAGING textures
      if ((MapType == D3D11_MAP_READ || MapType == D3D11_MAP_READ_WRITE) &&
          desc.Usage == D3D11_USAGE_STAGING) {
        // Calculate checksum of the data
        uint32_t checksum = calculateTextureChecksum(pMappedResource->pData,
                                                      pMappedResource->RowPitch,
                                                      desc.Width, desc.Height, desc.Format);

        std::ostringstream oss;
        oss << "[" << getLogTimestamp() << "] Map"
            << " type=" << mapTypeToString(MapType)
            << " res=0x" << std::hex << pResource << std::dec
            << " sub=" << Subresource
            << " dim=" << desc.Width << "x" << desc.Height
            << " usage=" << usageToString(desc.Usage)
            << " cpu=0x" << std::hex << desc.CPUAccessFlags << std::dec
            << " bind=0x" << std::hex << desc.BindFlags << std::dec
            << " fmt=" << desc.Format
            << " checksum=0x" << std::hex << checksum << std::dec;
        writeTraceLog(oss.str());

        // Track this texture for Unmap logging
        trackStagingTexture(pResource);
      }

      // Log Map(WRITE_DISCARD) on DYNAMIC textures (512x512, format 90)
      if (MapType == D3D11_MAP_WRITE_DISCARD &&
          desc.Usage == D3D11_USAGE_DYNAMIC &&
          desc.Width == 512 && desc.Height == 512 && desc.Format == 90) {
        std::ostringstream oss;
        oss << "[" << getLogTimestamp() << "] Map"
            << " type=" << mapTypeToString(MapType)
            << " res=0x" << std::hex << pResource << std::dec
            << " sub=" << Subresource
            << " dim=" << desc.Width << "x" << desc.Height
            << " usage=" << usageToString(desc.Usage)
            << " cpu=0x" << std::hex << desc.CPUAccessFlags << std::dec
            << " bind=0x" << std::hex << desc.BindFlags << std::dec
            << " fmt=" << desc.Format;
        writeTraceLog(oss.str());

        // Track this texture for Unmap checksum calculation
        trackStagingTexture(pResource);
        trackMappedTextureData(pResource, pMappedResource->pData, pMappedResource->RowPitch,
                                desc.Width, desc.Height, desc.Format);
      }

      tex->Release();
    }
  }

  return hr;
}

void STDMETHODCALLTYPE ID3D11DeviceContext_Unmap(
        ID3D11DeviceContext*      pContext,
        ID3D11Resource*           pResource,
        UINT                      Subresource) {
  auto procs = getContextProcs(pContext);

  // Log Unmap on tracked textures (STAGING and DYNAMIC) (if logging active)
  // IMPORTANT: Calculate checksum BEFORE calling real Unmap (while data is still mapped)
  if (isTraceLoggingActive() && pResource) {
    if (isStagingTextureTracked(pResource)) {
      std::ostringstream oss;
      oss << "[" << getLogTimestamp() << "] Unmap"
          << " res=0x" << std::hex << pResource << std::dec
          << " sub=" << Subresource;

      // Check if we have tracked mapped data (for WRITE_DISCARD operations)
      uint32_t checksum = getAndClearMappedChecksum(pResource);
      if (checksum != 0) {
        oss << " checksum=0x" << std::hex << checksum << std::dec;
      }

      writeTraceLog(oss.str());

      // Remove from tracking (unmap completes the Map/Unmap pair)
      untrackStagingTexture(pResource);
    }
  }

  procs->Unmap(pContext, pResource, Subresource);
}


void STDMETHODCALLTYPE ID3D11DeviceContext_CopyResource(
        ID3D11DeviceContext*      pContext,
        ID3D11Resource*           pDstResource,
        ID3D11Resource*           pSrcResource) {
  auto procs = getContextProcs(pContext);
  procs->CopyResource(pContext, pDstResource, pSrcResource);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_CopySubresourceRegion(
        ID3D11DeviceContext*      pContext,
        ID3D11Resource*           pDstResource,
        UINT                      DstSubresource,
        UINT                      DstX,
        UINT                      DstY,
        UINT                      DstZ,
        ID3D11Resource*           pSrcResource,
        UINT                      SrcSubresource,
  const D3D11_BOX*                pSrcBox) {
  auto procs = getContextProcs(pContext);

  // Log Arland pattern copies (if logging active)
  if (isTraceLoggingActive() && pDstResource && pSrcResource) {
    D3D11_RESOURCE_DIMENSION dstDim, srcDim;
    pDstResource->GetType(&dstDim);
    pSrcResource->GetType(&srcDim);

    if (dstDim == D3D11_RESOURCE_DIMENSION_TEXTURE2D && srcDim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
      ID3D11Texture2D* dstTex = nullptr;
      ID3D11Texture2D* srcTex = nullptr;
      pDstResource->QueryInterface(IID_PPV_ARGS(&dstTex));
      pSrcResource->QueryInterface(IID_PPV_ARGS(&srcTex));

      D3D11_TEXTURE2D_DESC dstDesc = {};
      D3D11_TEXTURE2D_DESC srcDesc = {};
      dstTex->GetDesc(&dstDesc);
      srcTex->GetDesc(&srcDesc);

      // Check if this matches the Arland lag pattern
      // Pattern: 512x512 DYNAMIC -> STAGING
      bool isArlandPattern = (srcDesc.Width == 512 && srcDesc.Height == 512 &&
                              srcDesc.Usage == D3D11_USAGE_DYNAMIC &&
                              srcDesc.CPUAccessFlags == 0x10000 &&
                              dstDesc.Usage == D3D11_USAGE_STAGING &&
                              dstDesc.CPUAccessFlags == 0x20000);

      if (isArlandPattern) {
        std::ostringstream oss;
        oss << "[" << getLogTimestamp() << "] CopySubresourceRegion"
            << " src=0x" << std::hex << pSrcResource << std::dec
            << " dst=0x" << std::hex << pDstResource << std::dec
            << " srcSub=" << SrcSubresource
            << " dstSub=" << DstSubresource
            << " srcDim=" << srcDesc.Width << "x" << srcDesc.Height
            << " dstDim=" << dstDesc.Width << "x" << dstDesc.Height
            << " srcUsage=" << usageToString(srcDesc.Usage)
            << " dstUsage=" << usageToString(dstDesc.Usage)
            << " srcCPU=0x" << std::hex << srcDesc.CPUAccessFlags << std::dec
            << " dstCPU=0x" << std::hex << dstDesc.CPUAccessFlags << std::dec
            << " srcBind=0x" << std::hex << srcDesc.BindFlags << std::dec
            << " dstBind=0x" << std::hex << dstDesc.BindFlags << std::dec
            << " fmt=" << srcDesc.Format
            << " dstPos=(" << DstX << "," << DstY << "," << DstZ << ")";

        // Add box info if present
        if (pSrcBox) {
          oss << " box=(" << pSrcBox->left << "," << pSrcBox->top << "," << pSrcBox->front
              << ")-(" << pSrcBox->right << "," << pSrcBox->bottom << "," << pSrcBox->back << ")"
              << " boxSize=" << (pSrcBox->right - pSrcBox->left) << "x" << (pSrcBox->bottom - pSrcBox->top);
        } else {
          oss << " box=full";
        }

        writeTraceLog(oss.str());
      }

      dstTex->Release();
      srcTex->Release();
    }
  }

  // Always do the actual GPU copy (no skipping)
  procs->CopySubresourceRegion(pContext, pDstResource, DstSubresource,
                                DstX, DstY, DstZ, pSrcResource, SrcSubresource, pSrcBox);
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

  log("Created hook for ", pName);
}

void hookDevice(ID3D11Device* pDevice) {
  log("=== hookDevice called ===");
}

void hookContext(ID3D11DeviceContext* pContext) {
  std::lock_guard lock(g_hookMutex);

  uint32_t flag = HOOK_IMM_CTX;
  ContextProcs* procs = &g_immContextProcs;

  if (!isImmediateContext(pContext)) {
    flag = HOOK_DEF_CTX;
    procs = &g_defContextProcs;
  }

  if (g_installedHooks & flag) {
    log("=== hookContext: Already hooked ===");
    return;
  }

  log("=== hookContext: Installing hooks ===");

  // Map/Unmap hooks (passthrough)
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 14, Map);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 15, Unmap);

  // Copy operation hooks for tracking
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 47, CopyResource);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 46, CopySubresourceRegion);

  g_installedHooks |= flag;

  /* Immediate context and deferred context methods may share code */
  if (flag & HOOK_IMM_CTX)
    g_defContextProcs = g_immContextProcs;

  // Start trace logging subsystem (only once, on first hook installation)
  static bool traceInitialized = false;
  if (!traceInitialized) {
    traceInitialized = true;
    initTraceLogging();
  }

  log("=== hookContext: Hooks installed successfully ===");
}

}
