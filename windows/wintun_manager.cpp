#include "wintun_manager.h"
#include <iostream>
#include <sstream>
#include <vector>
#include <shlwapi.h>
#include <objbase.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")

namespace openvpn_flutter {

WinTunManager::WinTunManager() {
    CoInitialize(NULL);
}

WinTunManager::~WinTunManager() {
    endSession();
    destroyAdapter();
    unloadWinTunDll();
    CoUninitialize();
}

bool WinTunManager::initialize() {
    if (!loadWinTunDll()) {
        std::cerr << "Failed to load WinTun.dll" << std::endl;
        return false;
    }
    
    if (!loadWinTunFunctions()) {
        std::cerr << "Failed to load WinTun functions" << std::endl;
        return false;
    }
    
    return true;
}

bool WinTunManager::isWinTunAvailable() {
    if (!wintunDll) {
        return loadWinTunDll();
    }
    return true;
}

bool WinTunManager::createAdapter(const std::string& name) {
    if (!wintunDll || !WinTunCreateAdapter) {
        std::cerr << "WinTun not initialized" << std::endl;
        return false;
    }
    
    adapterName = name.empty() ? generateAdapterName() : name;
    adapterGuid = generateGuid();
    
    // Convert adapter name to wide string
    std::wstring wAdapterName(adapterName.begin(), adapterName.end());
    std::wstring wTunnelType = L"OpenVPN";
    
    adapter = WinTunCreateAdapter(wAdapterName.c_str(), wTunnelType.c_str(), &adapterGuid);
    
    if (!adapter) {
        DWORD error = GetLastError();
        std::cerr << "Failed to create WinTun adapter. Error: " << error << std::endl;
        return false;
    }
    
    std::cout << "WinTun adapter created successfully: " << adapterName << std::endl;
    return true;
}

bool WinTunManager::destroyAdapter() {
    if (adapter && WinTunCloseAdapter) {
        BOOL result = WinTunCloseAdapter(adapter);
        adapter = NULL;
        return result == TRUE;
    }
    return true;
}

bool WinTunManager::startSession() {
    if (!adapter || !WinTunStartSession) {
        std::cerr << "WinTun adapter not available" << std::endl;
        return false;
    }
    
    // Start session with 0x400000 (4MB) ring buffer capacity
    session = WinTunStartSession(adapter, 0x400000);
    
    if (!session) {
        DWORD error = GetLastError();
        std::cerr << "Failed to start WinTun session. Error: " << error << std::endl;
        return false;
    }
    
    std::cout << "WinTun session started successfully" << std::endl;
    return true;
}

void WinTunManager::endSession() {
    if (session && WinTunEndSession) {
        WinTunEndSession(session);
        session = NULL;
    }
}

std::string WinTunManager::getAdapterName() const {
    return adapterName;
}

DWORD WinTunManager::getDriverVersion() {
    if (WinTunGetRunningDriverVersion) {
        return WinTunGetRunningDriverVersion();
    }
    return 0;
}

bool WinTunManager::loadWinTunDll() {
    if (wintunDll) {
        return true; // Already loaded
    }
    
    // Try to load WinTun.dll from various locations
    std::vector<std::string> possiblePaths = {
        "wintun.dll",                    // Current directory
        ".\\bin\\wintun.dll",           // bin subdirectory
        ".\\wintun\\wintun.dll",        // wintun subdirectory
    };
    
    // Get application directory and add to search paths
    char appPath[MAX_PATH];
    if (GetModuleFileNameA(NULL, appPath, MAX_PATH)) {
        PathRemoveFileSpecA(appPath);
        std::string appDir(appPath);
        possiblePaths.insert(possiblePaths.begin(), {
            appDir + "\\wintun.dll",
            appDir + "\\bin\\wintun.dll",
            appDir + "\\wintun\\wintun.dll"
        });
    }
    
    for (const auto& path : possiblePaths) {
        wintunDll = LoadLibraryA(path.c_str());
        if (wintunDll) {
            std::cout << "WinTun.dll loaded from: " << path << std::endl;
            return true;
        }
    }
    
    DWORD error = GetLastError();
    std::cerr << "Failed to load WinTun.dll. Error: " << error << std::endl;
    std::cerr << "Make sure wintun.dll is available in your application directory" << std::endl;
    return false;
}

void WinTunManager::unloadWinTunDll() {
    if (wintunDll) {
        FreeLibrary(wintunDll);
        wintunDll = NULL;
        
        // Clear function pointers
        WinTunCreateAdapter = nullptr;
        WinTunCloseAdapter = nullptr;
        WinTunStartSession = nullptr;
        WinTunEndSession = nullptr;
        WinTunGetRunningDriverVersion = nullptr;
    }
}

bool WinTunManager::loadWinTunFunctions() {
    if (!wintunDll) {
        return false;
    }
    
    // Load required WinTun functions
    WinTunCreateAdapter = reinterpret_cast<WINTUN_CREATE_ADAPTER_FUNC>(
        GetProcAddress(wintunDll, "WinTunCreateAdapter"));
    
    WinTunCloseAdapter = reinterpret_cast<WINTUN_CLOSE_ADAPTER_FUNC>(
        GetProcAddress(wintunDll, "WinTunCloseAdapter"));
    
    WinTunStartSession = reinterpret_cast<WINTUN_START_SESSION_FUNC>(
        GetProcAddress(wintunDll, "WinTunStartSession"));
    
    WinTunEndSession = reinterpret_cast<WINTUN_END_SESSION_FUNC>(
        GetProcAddress(wintunDll, "WinTunEndSession"));
    
    WinTunGetRunningDriverVersion = reinterpret_cast<WINTUN_GET_RUNNING_DRIVER_VERSION_FUNC>(
        GetProcAddress(wintunDll, "WinTunGetRunningDriverVersion"));
    
    // Check if all required functions were loaded
    if (!WinTunCreateAdapter || !WinTunCloseAdapter || 
        !WinTunStartSession || !WinTunEndSession) {
        std::cerr << "Failed to load required WinTun functions" << std::endl;
        return false;
    }
    
    std::cout << "WinTun functions loaded successfully" << std::endl;
    return true;
}

std::string WinTunManager::generateAdapterName() {
    // Generate a unique adapter name
    std::ostringstream oss;
    oss << "OpenVPN-WinTun-" << GetTickCount();
    return oss.str();
}

GUID WinTunManager::generateGuid() {
    GUID guid;
    if (SUCCEEDED(CoCreateGuid(&guid))) {
        return guid;
    }
    
    // Fallback: create a deterministic GUID based on current time
    GUID fallbackGuid = {0};
    DWORD tick = GetTickCount();
    fallbackGuid.Data1 = tick;
    fallbackGuid.Data2 = static_cast<WORD>(tick >> 16);
    fallbackGuid.Data3 = static_cast<WORD>(tick & 0xFFFF);
    
    return fallbackGuid;
}

} // namespace openvpn_flutter 