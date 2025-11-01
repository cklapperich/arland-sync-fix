#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>
#include <unordered_map>

#include "trace.h"
#include "util.h"
#include "log.h"

namespace atfix {

extern Log log;

// Per-call trace logging (F9 toggle)
static std::atomic<bool> g_loggingActive = false;
static mutex g_logMutex;
static std::ofstream g_traceLog;
static auto g_logStartTime = std::chrono::high_resolution_clock::now();

// Track which textures we're interested in (STAGING for reads, DYNAMIC for writes)
static mutex g_stagingTexMutex;
static std::unordered_map<void*, bool> g_trackedStagingTextures;

// Track mapped data for Unmap checksum calculation (for WRITE operations)
struct MappedTextureData {
  void* pData;
  UINT rowPitch;
  UINT width;
  UINT height;
  DXGI_FORMAT format;
};
static mutex g_mappedDataMutex;
static std::unordered_map<void*, MappedTextureData> g_trackedMappedData;

// Background thread for F9 polling
static std::atomic<bool> g_shutdownThread = false;
static std::thread g_hotkeyThread;

const char* usageToString(D3D11_USAGE Usage) {
  switch (Usage) {
    case D3D11_USAGE_DEFAULT:   return "DEFAULT";
    case D3D11_USAGE_IMMUTABLE: return "IMMUTABLE";
    case D3D11_USAGE_DYNAMIC:   return "DYNAMIC";
    case D3D11_USAGE_STAGING:   return "STAGING";
    default:                    return "UNKNOWN";
  }
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

std::string getLogTimestamp() {
  auto now = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(now - g_logStartTime);
  return std::to_string(duration.count());
}

void writeTraceLog(const std::string& line) {
  std::lock_guard lock(g_logMutex);
  if (g_loggingActive && g_traceLog.is_open()) {
    g_traceLog << line << std::endl;
    g_traceLog.flush();  // Flush immediately for real-time analysis
  }
}

bool isTraceLoggingActive() {
  return g_loggingActive;
}

void trackStagingTexture(void* pResource) {
  std::lock_guard lock(g_stagingTexMutex);
  g_trackedStagingTextures[pResource] = true;
}

void untrackStagingTexture(void* pResource) {
  std::lock_guard lock(g_stagingTexMutex);
  g_trackedStagingTextures.erase(pResource);
}

bool isStagingTextureTracked(void* pResource) {
  std::lock_guard lock(g_stagingTexMutex);
  return g_trackedStagingTextures.find(pResource) != g_trackedStagingTextures.end();
}

void trackMappedTextureData(void* pResource, const void* pData, UINT rowPitch, UINT width, UINT height, DXGI_FORMAT format) {
  std::lock_guard lock(g_mappedDataMutex);
  g_trackedMappedData[pResource] = {const_cast<void*>(pData), rowPitch, width, height, format};
}

uint32_t getAndClearMappedChecksum(void* pResource) {
  std::lock_guard lock(g_mappedDataMutex);
  auto it = g_trackedMappedData.find(pResource);
  if (it != g_trackedMappedData.end()) {
    auto& data = it->second;
    uint32_t checksum = calculateTextureChecksum(data.pData, data.rowPitch, data.width, data.height, data.format);
    g_trackedMappedData.erase(it);  // Remove after checksum (Unmap completes the pair)
    return checksum;
  }
  return 0;
}

void hotkeyPollingThread() {
  log(">>> Hotkey polling thread started <<<");

  bool lastF9State = false;

  while (!g_shutdownThread) {
    // Check for F9 key (VK_F9 = 0x78)
    bool f9Pressed = (GetAsyncKeyState(0x78) & 0x8000) != 0;

    // Detect rising edge (key just pressed)
    if (f9Pressed && !lastF9State) {
      std::lock_guard lock(g_logMutex);

      if (!g_loggingActive) {
        // START logging
        log("=== F9 PRESSED - STARTING TRACE LOGGING ===");

        // Close existing log if open
        if (g_traceLog.is_open()) {
          g_traceLog.close();
        }

        // Open fresh log file (truncate mode)
        g_traceLog.open("atfix_trace.log", std::ios::out | std::ios::trunc);
        if (!g_traceLog.is_open()) {
          log("ERROR: Failed to open atfix_trace.log");
        } else {
          // Reset timestamp reference
          g_logStartTime = std::chrono::high_resolution_clock::now();

          // Clear tracked textures
          {
            std::lock_guard texLock(g_stagingTexMutex);
            g_trackedStagingTextures.clear();
          }
          {
            std::lock_guard dataLock(g_mappedDataMutex);
            g_trackedMappedData.clear();
          }

          g_loggingActive = true;
          log(">>> LOGGING STARTED - trace written to atfix_trace.log <<<");

          // Write header
          g_traceLog << "# atfix trace log - timestamps in microseconds" << std::endl;
          g_traceLog << "# Format: [timestamp_us] CallType key=value ..." << std::endl;
          g_traceLog.flush();
        }

      } else {
        // STOP logging
        log("=== F9 PRESSED - STOPPING TRACE LOGGING ===");
        g_loggingActive = false;

        if (g_traceLog.is_open()) {
          g_traceLog.close();
          log(">>> LOGGING STOPPED - trace saved to atfix_trace.log <<<");
        }
      }
    }

    lastF9State = f9Pressed;

    // Sleep to avoid busy-waiting (poll every 50ms)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  log(">>> Hotkey polling thread exiting <<<");
}

void initTraceLogging() {
  g_shutdownThread = false;
  g_hotkeyThread = std::thread(hotkeyPollingThread);
  log("=== Trace logging initialized - Press F9 to start/stop ===");
}

void shutdownTraceLogging() {
  g_shutdownThread = true;
  if (g_hotkeyThread.joinable()) {
    g_hotkeyThread.join();
  }

  // Close log file if still open
  std::lock_guard lock(g_logMutex);
  if (g_traceLog.is_open()) {
    g_traceLog.close();
  }
}

uint32_t calculateTextureChecksum(const void* pData, UINT rowPitch, UINT width, UINT height, DXGI_FORMAT format) {
  if (!pData) return 0;

  // Simple CRC32-like checksum using XOR and rotation
  uint32_t checksum = 0x12345678;
  const uint8_t* bytes = static_cast<const uint8_t*>(pData);

  // Determine bytes per pixel based on format
  // For now, handle common formats (format 90 is likely DXGI_FORMAT_B8G8R8A8_UNORM = 4 bytes/pixel)
  UINT bytesPerPixel = 4;  // Default assumption for most common formats

  // Calculate how many bytes to read per row (minimum of actual data and pitch)
  UINT bytesPerRow = width * bytesPerPixel;

  for (UINT row = 0; row < height; row++) {
    const uint8_t* rowData = bytes + (row * rowPitch);

    for (UINT col = 0; col < bytesPerRow; col++) {
      // Simple hash: rotate left and XOR
      checksum = ((checksum << 5) | (checksum >> 27)) ^ rowData[col];
    }
  }

  return checksum;
}

}
