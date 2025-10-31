#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>

#include "impl.h"
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

// Statistics tracking for Option 2
// Copy operation signature - tracks SRC->DST pattern
struct CopySignature {
  // Source texture
  uint32_t srcWidth = 0;
  uint32_t srcHeight = 0;
  DXGI_FORMAT srcFormat = DXGI_FORMAT_UNKNOWN;
  D3D11_USAGE srcUsage = D3D11_USAGE_DEFAULT;
  uint32_t srcCPUAccessFlags = 0;
  uint32_t srcBindFlags = 0;

  // Destination texture
  uint32_t dstWidth = 0;
  uint32_t dstHeight = 0;
  DXGI_FORMAT dstFormat = DXGI_FORMAT_UNKNOWN;
  D3D11_USAGE dstUsage = D3D11_USAGE_DEFAULT;
  uint32_t dstCPUAccessFlags = 0;
  uint32_t dstBindFlags = 0;

  bool operator==(const CopySignature& other) const {
    return srcWidth == other.srcWidth &&
           srcHeight == other.srcHeight &&
           srcFormat == other.srcFormat &&
           srcUsage == other.srcUsage &&
           srcCPUAccessFlags == other.srcCPUAccessFlags &&
           srcBindFlags == other.srcBindFlags &&
           dstWidth == other.dstWidth &&
           dstHeight == other.dstHeight &&
           dstFormat == other.dstFormat &&
           dstUsage == other.dstUsage &&
           dstCPUAccessFlags == other.dstCPUAccessFlags &&
           dstBindFlags == other.dstBindFlags;
  }
};

// Hash function for CopySignature
struct CopySignatureHash {
  size_t operator()(const CopySignature& sig) const {
    size_t h = 0;
    h ^= std::hash<uint32_t>{}(sig.srcWidth) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<uint32_t>{}(sig.srcHeight) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<int>{}(sig.srcFormat) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<int>{}(sig.srcUsage) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<uint32_t>{}(sig.srcCPUAccessFlags) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<uint32_t>{}(sig.srcBindFlags) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<uint32_t>{}(sig.dstWidth) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<uint32_t>{}(sig.dstHeight) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<int>{}(sig.dstFormat) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<int>{}(sig.dstUsage) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<uint32_t>{}(sig.dstCPUAccessFlags) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<uint32_t>{}(sig.dstBindFlags) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
  }
};

struct CopyStats {
  uint64_t count = 0;
};

static mutex g_statsMutex;
static std::unordered_map<CopySignature, CopyStats, CopySignatureHash> g_copyStats;
static std::atomic<uint64_t> g_totalCopies = 0;

// Diagnostic counters
static std::atomic<uint64_t> g_allCopySubresourceCalls = 0;
static std::atomic<uint64_t> g_tex2dCopies = 0;
static std::atomic<uint64_t> g_stagingDstCopies = 0;
static std::atomic<uint64_t> g_dynamicSrcCopies = 0;

// Timing for periodic reports
static auto g_startTime = std::chrono::high_resolution_clock::now();
static auto g_lastReportTime = std::chrono::high_resolution_clock::now();
static constexpr double REPORT_INTERVAL_SECONDS = 1.0;  // Update every second

// Statistics only - no skipping
static std::atomic<uint64_t> g_arlandPatternCounter = 0;

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
  oss << std::fixed << std::setprecision(3) << seconds;
  return oss.str();
}

const char* usageToString(D3D11_USAGE Usage) {
  switch (Usage) {
    case D3D11_USAGE_DEFAULT:   return "DEFAULT";
    case D3D11_USAGE_IMMUTABLE: return "IMMUTABLE";
    case D3D11_USAGE_DYNAMIC:   return "DYNAMIC";
    case D3D11_USAGE_STAGING:   return "STAGING";
    default:                    return "UNKNOWN";
  }
}


void printStatisticsReport() {
  std::lock_guard lock(g_statsMutex);

  uint64_t totalCopies = g_totalCopies.load();

  // Open stats file in OVERWRITE mode (truncates each time)
  std::ofstream statsFile("atfix_stats.log", std::ios::out | std::ios::trunc);
  if (!statsFile.is_open()) {
    return;
  }

  statsFile << "=== TEXTURE COPY/READ STATISTICS (" << getTimestamp() << "s) ===" << std::endl;
  statsFile << std::endl;
  statsFile << "DIAGNOSTICS:" << std::endl;
  statsFile << "  All CopySubresourceRegion calls: " << g_allCopySubresourceCalls.load() << std::endl;
  statsFile << "  Tex2D->Tex2D copies: " << g_tex2dCopies.load() << std::endl;
  statsFile << "  Copies with STAGING dst: " << g_stagingDstCopies.load() << std::endl;
  statsFile << "  Copies with DYNAMIC src: " << g_dynamicSrcCopies.load() << std::endl;
  statsFile << std::endl;
  statsFile << "ARLAND PATTERN:" << std::endl;
  statsFile << "  Arland pattern (512x512 DYNAMIC->STAGING) copies: " << g_arlandPatternCounter.load() << std::endl;
  statsFile << std::endl;
  statsFile << "TRACKING:" << std::endl;
  statsFile << "  Total Tex2D copies tracked: " << totalCopies << std::endl;
  statsFile << std::endl;
  statsFile << "Copy patterns (" << g_copyStats.size() << " unique patterns):" << std::endl;

  // Sort by copy count (descending) to show most frequent patterns first
  struct Entry {
    CopySignature sig;
    CopyStats stats;
  };
  std::vector<Entry> sorted;
  sorted.reserve(g_copyStats.size());
  for (const auto& [sig, stats] : g_copyStats) {
    sorted.push_back({sig, stats});
  }
  std::sort(sorted.begin(), sorted.end(), [](const Entry& a, const Entry& b) {
    return a.stats.count > b.stats.count;
  });

  for (const auto& entry : sorted) {
    const auto& sig = entry.sig;
    const auto& stats = entry.stats;

    statsFile << "  [" << sig.srcWidth << "x" << sig.srcHeight << " "
              << usageToString(sig.srcUsage) << " cpu=0x" << std::hex << sig.srcCPUAccessFlags
              << " -> " << std::dec << sig.dstWidth << "x" << sig.dstHeight << " "
              << usageToString(sig.dstUsage) << " cpu=0x" << std::hex << sig.dstCPUAccessFlags << std::dec
              << "]: count=" << stats.count << std::endl;
  }

  statsFile << "==========================================================" << std::endl;
  statsFile.close();
}

void maybeReportStatistics() {
  auto now = std::chrono::high_resolution_clock::now();
  auto elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(now - g_lastReportTime);

  if (elapsed.count() >= REPORT_INTERVAL_SECONDS) {
    printStatisticsReport();
    g_lastReportTime = now;
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
  return procs->Map(pContext, pResource, Subresource, MapType, MapFlags, pMappedResource);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_Unmap(
        ID3D11DeviceContext*      pContext,
        ID3D11Resource*           pResource,
        UINT                      Subresource) {
  auto procs = getContextProcs(pContext);
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

  uint64_t callNum = ++g_allCopySubresourceCalls;

  // Log first few calls to verify hook is working
  if (callNum <= 5) {
    log("CopySubresourceRegion hook called! Call #", callNum);
  }

  // Track Arland pattern: DYNAMIC WRITE → STAGING READ
  if (pDstResource && pSrcResource) {
    D3D11_RESOURCE_DIMENSION dstDim, srcDim;
    pDstResource->GetType(&dstDim);
    pSrcResource->GetType(&srcDim);

    if (dstDim == D3D11_RESOURCE_DIMENSION_TEXTURE2D && srcDim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
      g_tex2dCopies++;

      ID3D11Texture2D* dstTex = nullptr;
      ID3D11Texture2D* srcTex = nullptr;
      pDstResource->QueryInterface(IID_PPV_ARGS(&dstTex));
      pSrcResource->QueryInterface(IID_PPV_ARGS(&srcTex));

      D3D11_TEXTURE2D_DESC dstDesc = {};
      D3D11_TEXTURE2D_DESC srcDesc = {};
      dstTex->GetDesc(&dstDesc);
      srcTex->GetDesc(&srcDesc);

      // Log first few Tex2D copies in detail
      if (callNum <= 10) {
        log("  Copy #", callNum, ": ",
            srcDesc.Width, "x", srcDesc.Height, " ",
            usageToString(srcDesc.Usage), " (cpu=0x", std::hex, srcDesc.CPUAccessFlags, ") -> ",
            dstDesc.Width, "x", dstDesc.Height, " ",
            usageToString(dstDesc.Usage), " (cpu=0x", dstDesc.CPUAccessFlags, ")", std::dec);
      }

      // Diagnostic: track how many copies match partial patterns
      if (dstDesc.Usage == D3D11_USAGE_STAGING) {
        g_stagingDstCopies++;
        if (callNum <= 10) log("    -> STAGING dst detected, count=", g_stagingDstCopies.load());
      }
      if (srcDesc.Usage == D3D11_USAGE_DYNAMIC) {
        g_dynamicSrcCopies++;
        if (callNum <= 10) log("    -> DYNAMIC src detected, count=", g_dynamicSrcCopies.load());
      }

      // Track ALL Tex2D copy patterns
      CopySignature copySig;
      copySig.srcWidth = srcDesc.Width;
      copySig.srcHeight = srcDesc.Height;
      copySig.srcFormat = srcDesc.Format;
      copySig.srcUsage = srcDesc.Usage;
      copySig.srcCPUAccessFlags = srcDesc.CPUAccessFlags;
      copySig.srcBindFlags = srcDesc.BindFlags;
      copySig.dstWidth = dstDesc.Width;
      copySig.dstHeight = dstDesc.Height;
      copySig.dstFormat = dstDesc.Format;
      copySig.dstUsage = dstDesc.Usage;
      copySig.dstCPUAccessFlags = dstDesc.CPUAccessFlags;
      copySig.dstBindFlags = dstDesc.BindFlags;

      // Check if this matches the Arland lag pattern
      // Pattern: 512x512 DYNAMIC -> NxN STAGING (destination size varies!)
      bool isArlandPattern = (srcDesc.Width == 512 && srcDesc.Height == 512 &&
                              srcDesc.Usage == D3D11_USAGE_DYNAMIC &&
                              srcDesc.CPUAccessFlags == 0x10000 &&
                              dstDesc.Usage == D3D11_USAGE_STAGING &&
                              dstDesc.CPUAccessFlags == 0x20000);

      if (isArlandPattern) {
        uint64_t patternNum = ++g_arlandPatternCounter;
        if (patternNum <= 20) {
          log("  Arland pattern copy #", patternNum, " (",
              dstDesc.Width, "x", dstDesc.Height, " dst)");
        }
      }

      std::lock_guard lock(g_statsMutex);
      auto it = g_copyStats.find(copySig);
      if (it != g_copyStats.end()) {
        it->second.count++;
      } else {
        CopyStats stats;
        stats.count = 1;
        g_copyStats[copySig] = stats;
      }
      g_totalCopies++;

      dstTex->Release();
      srcTex->Release();
    }
  }

  // Always do the actual GPU copy (no skipping)
  procs->CopySubresourceRegion(pContext, pDstResource, DstSubresource,
                                DstX, DstY, DstZ, pSrcResource, SrcSubresource, pSrcBox);

  // Update stats periodically
  maybeReportStatistics();
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

  log("=== hookContext: Hooks installed successfully ===");

  // Write initial empty statistics report to verify file creation works
  printStatisticsReport();
  log("=== Initial statistics report written to atfix_stats.log ===");
}

}
