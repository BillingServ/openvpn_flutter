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
        // Try loading with LOAD_WITH_ALTERED_SEARCH_PATH to help with dependencies
        wintunDll = LoadLibraryExA(path.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!wintunDll) {
            // Fallback to regular LoadLibrary
            wintunDll = LoadLibraryA(path.c_str());
        }
        if (wintunDll) {
            std::cout << "WinTun.dll loaded from: " << path << std::endl;
            // Verify DLL is valid by checking if we can get module handle
            HMODULE verifyHandle = GetModuleHandleA(path.c_str());
            if (verifyHandle != wintunDll) {
                std::cerr << "Warning: DLL handle verification failed" << std::endl;
            }
            return true;
        } else {
            DWORD error = GetLastError();
            std::cerr << "Failed to load from " << path << ". Error: " << error << std::endl;
        }
    }
    
    DWORD error = GetLastError();
    std::cerr << "Failed to load WinTun.dll from all attempted paths. Last error: " << error << std::endl;
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
    
    // Load required WinTun functions with detailed error reporting
    WinTunCreateAdapter = reinterpret_cast<WINTUN_CREATE_ADAPTER_FUNC>(
        GetProcAddress(wintunDll, "WinTunCreateAdapter"));
    if (!WinTunCreateAdapter) {
        DWORD error = GetLastError();
        std::cerr << "Failed to load WinTunCreateAdapter. Error: " << error << std::endl;
    }
    
    WinTunCloseAdapter = reinterpret_cast<WINTUN_CLOSE_ADAPTER_FUNC>(
        GetProcAddress(wintunDll, "WinTunCloseAdapter"));
    if (!WinTunCloseAdapter) {
        DWORD error = GetLastError();
        std::cerr << "Failed to load WinTunCloseAdapter. Error: " << error << std::endl;
    }
    
    WinTunStartSession = reinterpret_cast<WINTUN_START_SESSION_FUNC>(
        GetProcAddress(wintunDll, "WinTunStartSession"));
    if (!WinTunStartSession) {
        DWORD error = GetLastError();
        std::cerr << "Failed to load WinTunStartSession. Error: " << error << std::endl;
    }
    
    WinTunEndSession = reinterpret_cast<WINTUN_END_SESSION_FUNC>(
        GetProcAddress(wintunDll, "WinTunEndSession"));
    if (!WinTunEndSession) {
        DWORD error = GetLastError();
        std::cerr << "Failed to load WinTunEndSession. Error: " << error << std::endl;
    }
    
    WinTunGetRunningDriverVersion = reinterpret_cast<WINTUN_GET_RUNNING_DRIVER_VERSION_FUNC>(
        GetProcAddress(wintunDll, "WinTunGetRunningDriverVersion"));
    if (!WinTunGetRunningDriverVersion) {
        DWORD error = GetLastError();
        std::cerr << "Failed to load WinTunGetRunningDriverVersion. Error: " << error << std::endl;
    }
    
    // Check if all required functions were loaded
    if (!WinTunCreateAdapter || !WinTunCloseAdapter || 
        !WinTunStartSession || !WinTunEndSession) {
        std::cerr << "Failed to load required WinTun functions" << std::endl;
        std::cerr << "WinTunCreateAdapter: " << (WinTunCreateAdapter ? "OK" : "FAILED") << std::endl;
        std::cerr << "WinTunCloseAdapter: " << (WinTunCloseAdapter ? "OK" : "FAILED") << std::endl;
        std::cerr << "WinTunStartSession: " << (WinTunStartSession ? "OK" : "FAILED") << std::endl;
        std::cerr << "WinTunEndSession: " << (WinTunEndSession ? "OK" : "FAILED") << std::endl;
        
        // Diagnostic: Check if DLL is valid by trying to get module filename
        char modulePath[MAX_PATH];
        if (GetModuleFileNameA(wintunDll, modulePath, MAX_PATH)) {
            std::cerr << "DLL module path: " << modulePath << std::endl;
        }
        
        // Note: GetProcAddress returns NULL if function not found, but doesn't set error code
        // This suggests the DLL might be wrong version, corrupted, or missing exports
        std::cerr << "Possible causes:" << std::endl;
        std::cerr << "  1. DLL version mismatch (wrong WinTun version)" << std::endl;
        std::cerr << "  2. DLL is corrupted or incomplete" << std::endl;
        std::cerr << "  3. DLL architecture mismatch (x86 vs x64)" << std::endl;
        std::cerr << "  4. Missing DLL dependencies" << std::endl;
        return false;
    }
    
    std::cout << "WinTun functions loaded successfully" << std::endl;
    return true;
}

std::string WinTunManager::generateAdapterName() {
    // Generate a unique adapter name
    return "OpenVPN-Flutter-" + std::to_string(GetTickCount64());
}

GUID WinTunManager::generateGuid() {
    GUID guid;
    if (SUCCEEDED(CoCreateGuid(&guid))) {
        return guid;
    }
    
    // Fallback: generate a simple GUID-like structure
    GUID fallbackGuid = {0};
    fallbackGuid.Data1 = GetTickCount();
    fallbackGuid.Data2 = static_cast<unsigned short>(rand());
    fallbackGuid.Data3 = static_cast<unsigned short>(rand());
    for (int i = 0; i < 8; i++) {
        fallbackGuid.Data4[i] = static_cast<unsigned char>(rand() % 256);
    }
    return fallbackGuid;
}

} // namespace openvpn_flutter 