// d3d11_minimal.cpp
#include <windows.h>
#include <d3d11.h>
#include <fstream>

// Function pointer for the real D3D11CreateDevice
typedef HRESULT (WINAPI *D3D11CreateDevice_t)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT,
    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

D3D11CreateDevice_t RealD3D11CreateDevice = nullptr;
HMODULE realD3D11 = nullptr;

void Log(const char* msg) {
    std::ofstream log("d3d11_log.txt", std::ios::app);
    log << msg << std::endl;
    log.close();
}

// DLL entry point
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        Log("=== Our d3d11.dll loaded! ===");
        
        // Load the real d3d11.dll from system
        char sysPath[MAX_PATH];
        GetSystemDirectoryA(sysPath, MAX_PATH);
        strcat(sysPath, "\\d3d11.dll");
        
        realD3D11 = LoadLibraryA(sysPath);
        if (realD3D11) {
            RealD3D11CreateDevice = (D3D11CreateDevice_t)
                GetProcAddress(realD3D11, "D3D11CreateDevice");
            Log("Real d3d11.dll loaded successfully");
        } else {
            Log("ERROR: Failed to load real d3d11.dll");
        }
    }
    return TRUE;
}

// Our intercepted function - just forwards to real one
extern "C" __declspec(dllexport) HRESULT WINAPI D3D11CreateDevice(
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
    Log("D3D11CreateDevice called!");
    
    if (RealD3D11CreateDevice) {
        return RealD3D11CreateDevice(pAdapter, DriverType, Software, Flags,
            pFeatureLevels, FeatureLevels, SDKVersion,
            ppDevice, pFeatureLevel, ppImmediateContext);
    }
    
    return E_FAIL;
}