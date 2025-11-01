#pragma once

#include <string>
#include <d3d11.h>

namespace atfix {

// Initialize trace logging subsystem (starts F9 hotkey polling thread)
void initTraceLogging();

// Shutdown trace logging subsystem (stops hotkey thread)
void shutdownTraceLogging();

// Check if trace logging is currently active
bool isTraceLoggingActive();

// Write a line to the trace log (only if logging is active)
void writeTraceLog(const std::string& line);

// Get timestamp string for trace log entries (microseconds since logging started)
std::string getLogTimestamp();

// Helper functions for converting D3D11 enums to strings
const char* usageToString(D3D11_USAGE Usage);
const char* mapTypeToString(D3D11_MAP MapType);

// Track staging textures for Map/Unmap correlation
void trackStagingTexture(void* pResource);
void untrackStagingTexture(void* pResource);
bool isStagingTextureTracked(void* pResource);

// Track mapped texture data for Unmap checksum calculation (for WRITE operations)
void trackMappedTextureData(void* pResource, const void* pData, UINT rowPitch, UINT width, UINT height, DXGI_FORMAT format);
uint32_t getAndClearMappedChecksum(void* pResource);

// Calculate checksum of mapped texture data
uint32_t calculateTextureChecksum(const void* pData, UINT rowPitch, UINT width, UINT height, DXGI_FORMAT format);

}
