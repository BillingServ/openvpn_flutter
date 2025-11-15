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
    
    // Get application directory
    char appPath[MAX_PATH];
    std::string appDir;
    if (GetModuleFileNameA(NULL, appPath, MAX_PATH)) {
        PathRemoveFileSpecA(appPath);
        appDir = std::string(appPath);
    }
    
    // CRITICAL: Add directories to DLL search path BEFORE loading wintun.dll
    // This ensures DLL dependencies (libcrypto, libssl) can be found
    // Windows searches for DLL dependencies in:
    // 1. Directory containing the executable
    // 2. System directories  
    // 3. Directories added via AddDllDirectory/SetDllDirectory
    // 4. Current directory
    // 5. PATH environment variable
    
    std::string binDir = appDir + "\\bin";
    std::vector<DLL_DIRECTORY_COOKIE> dllDirCookies;
    
    // Use AddDllDirectory (Windows Vista+) to add directories without replacing default search
    typedef BOOL (WINAPI *AddDllDirectoryFunc)(PCWSTR);
    typedef BOOL (WINAPI *RemoveDllDirectoryFunc)(DLL_DIRECTORY_COOKIE);
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    AddDllDirectoryFunc addDllDir = nullptr;
    RemoveDllDirectoryFunc removeDllDir = nullptr;
    
    if (kernel32) {
        addDllDir = (AddDllDirectoryFunc)GetProcAddress(kernel32, "AddDllDirectory");
        removeDllDir = (RemoveDllDirectoryFunc)GetProcAddress(kernel32, "RemoveDllDirectory");
    }
    
    // Add bin directory if it exists
    if (!appDir.empty() && PathFileExistsA(binDir.c_str())) {
        if (addDllDir) {
            // Convert to wide string for AddDllDirectory
            int wlen = MultiByteToWideChar(CP_ACP, 0, binDir.c_str(), -1, NULL, 0);
            std::vector<wchar_t> wbinDir(wlen);
            MultiByteToWideChar(CP_ACP, 0, binDir.c_str(), -1, &wbinDir[0], wlen);
            DLL_DIRECTORY_COOKIE cookie = addDllDir(&wbinDir[0]);
            if (cookie) {
                dllDirCookies.push_back(cookie);
                std::cout << "Added bin directory to DLL search path: " << binDir << std::endl;
            }
        } else {
            // Fallback to SetDllDirectory for older Windows
            SetDllDirectoryA(binDir.c_str());
            std::cout << "Added bin directory to DLL search path (via SetDllDirectory): " << binDir << std::endl;
        }
    }
    
    // Also add app directory (where dependencies might also be)
    if (!appDir.empty() && addDllDir) {
        int wlen = MultiByteToWideChar(CP_ACP, 0, appDir.c_str(), -1, NULL, 0);
        std::vector<wchar_t> wappDir(wlen);
        MultiByteToWideChar(CP_ACP, 0, appDir.c_str(), -1, &wappDir[0], wlen);
        DLL_DIRECTORY_COOKIE cookie = addDllDir(&wappDir[0]);
        if (cookie) {
            dllDirCookies.push_back(cookie);
            std::cout << "Added app directory to DLL search path: " << appDir << std::endl;
        }
    }
    
    // Try to load WinTun.dll from various locations
    // Prefer app directory first (dependencies should be there too)
    std::vector<std::string> possiblePaths = {
        appDir + "\\wintun.dll",         // App directory (preferred - dependencies nearby)
        appDir + "\\bin\\wintun.dll",    // Bin subdirectory
        "wintun.dll",                    // Current directory
        ".\\bin\\wintun.dll",           // bin subdirectory
        ".\\wintun\\wintun.dll",        // wintun subdirectory
    };
    
    for (const auto& path : possiblePaths) {
        // Use LOAD_WITH_ALTERED_SEARCH_PATH to search in DLL's directory first
        // This ensures dependencies in the same directory as wintun.dll are found
        wintunDll = LoadLibraryExA(path.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!wintunDll) {
            // Fallback to regular LoadLibrary (will use AddDllDirectory paths)
            wintunDll = LoadLibraryA(path.c_str());
        }
        if (wintunDll) {
            std::cout << "WinTun.dll loaded from: " << path << std::endl;
            
            // Verify DLL loaded correctly by checking if we can get a proc address
            // This helps catch cases where DLL loads but isn't initialized
            FARPROC testProc = GetProcAddress(wintunDll, "DllMain");
            if (!testProc) {
                std::cerr << "Warning: DLL loaded but DllMain not found - DLL may not be initialized" << std::endl;
            }
            
            return true;
        } else {
            DWORD error = GetLastError();
            std::cerr << "Failed to load from " << path << ". Error: " << error << std::endl;
        }
    }
    
    // Clean up DLL directory cookies
    if (removeDllDir) {
        for (auto cookie : dllDirCookies) {
            removeDllDir(cookie);
        }
    } else {
        // Fallback: restore DLL search path
        SetDllDirectoryA(NULL);
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
    
    // Diagnostic: Try to get DLL info
    char modulePath[MAX_PATH];
    if (GetModuleFileNameA(wintunDll, modulePath, MAX_PATH)) {
        std::cout << "Attempting to load functions from: " << modulePath << std::endl;
    }
    
    // Verify DLL is valid by checking if we can get any export
    // Try to get the DLL's entry point or any common export
    FARPROC testProc = GetProcAddress(wintunDll, "DllGetClassObject");
    if (testProc) {
        std::cout << "DLL appears to be a COM DLL (unexpected for WinTun)" << std::endl;
    }
    
    // Check DLL version info if available
    // Note: This is a diagnostic to help identify the DLL
    
    // Diagnostic: Try to enumerate some common exports to verify DLL type
    const char* testExports[] = {
        "DllMain", "DllGetClassObject", "DllCanUnloadNow", "DllRegisterServer",
        "WinTunCreateAdapter", "WintunCreateAdapter", "wintun_create_adapter"
    };
    std::cout << "Checking DLL exports..." << std::endl;
    bool foundAnyExport = false;
    for (const char* exportName : testExports) {
        FARPROC proc = GetProcAddress(wintunDll, exportName);
        if (proc) {
            std::cout << "Found export: " << exportName << std::endl;
            foundAnyExport = true;
        }
    }
    if (!foundAnyExport) {
        std::cerr << "WARNING: Could not find any common exports in DLL!" << std::endl;
        std::cerr << "This suggests the DLL might not be a valid WinTun DLL or is corrupted." << std::endl;
    }
    
    // Try loading with both ANSI and Unicode function name variants
    // Some DLLs export functions with different name formats
    const char* functionNames[] = {
        "WinTunCreateAdapter",
        "_WinTunCreateAdapter@12",  // __stdcall decorated name
        "WinTunCreateAdapterA",     // ANSI variant
        "WinTunCreateAdapterW"     // Unicode variant
    };
    
    // Load required WinTun functions with detailed error reporting
    WinTunCreateAdapter = reinterpret_cast<WINTUN_CREATE_ADAPTER_FUNC>(
        GetProcAddress(wintunDll, "WinTunCreateAdapter"));
    if (!WinTunCreateAdapter) {
        // Try alternative names
        for (int i = 1; i < 4 && !WinTunCreateAdapter; i++) {
            WinTunCreateAdapter = reinterpret_cast<WINTUN_CREATE_ADAPTER_FUNC>(
                GetProcAddress(wintunDll, functionNames[i]));
            if (WinTunCreateAdapter) {
                std::cout << "Found WinTunCreateAdapter with name: " << functionNames[i] << std::endl;
                break;
            }
        }
    }
    if (!WinTunCreateAdapter) {
        DWORD error = GetLastError();
        std::cerr << "Failed to load WinTunCreateAdapter. Error: " << error << std::endl;
        std::cerr << "Tried names: WinTunCreateAdapter, _WinTunCreateAdapter@12, WinTunCreateAdapterA, WinTunCreateAdapterW" << std::endl;
    }
    
    WinTunCloseAdapter = reinterpret_cast<WINTUN_CLOSE_ADAPTER_FUNC>(
        GetProcAddress(wintunDll, "WinTunCloseAdapter"));
    if (!WinTunCloseAdapter) {
        WinTunCloseAdapter = reinterpret_cast<WINTUN_CLOSE_ADAPTER_FUNC>(
            GetProcAddress(wintunDll, "_WinTunCloseAdapter@4"));
    }
    if (!WinTunCloseAdapter) {
        DWORD error = GetLastError();
        std::cerr << "Failed to load WinTunCloseAdapter. Error: " << error << std::endl;
    }
    
    WinTunStartSession = reinterpret_cast<WINTUN_START_SESSION_FUNC>(
        GetProcAddress(wintunDll, "WinTunStartSession"));
    if (!WinTunStartSession) {
        WinTunStartSession = reinterpret_cast<WINTUN_START_SESSION_FUNC>(
            GetProcAddress(wintunDll, "_WinTunStartSession@8"));
    }
    if (!WinTunStartSession) {
        DWORD error = GetLastError();
        std::cerr << "Failed to load WinTunStartSession. Error: " << error << std::endl;
    }
    
    WinTunEndSession = reinterpret_cast<WINTUN_END_SESSION_FUNC>(
        GetProcAddress(wintunDll, "WinTunEndSession"));
    if (!WinTunEndSession) {
        WinTunEndSession = reinterpret_cast<WINTUN_END_SESSION_FUNC>(
            GetProcAddress(wintunDll, "_WinTunEndSession@4"));
    }
    if (!WinTunEndSession) {
        DWORD error = GetLastError();
        std::cerr << "Failed to load WinTunEndSession. Error: " << error << std::endl;
    }
    
    WinTunGetRunningDriverVersion = reinterpret_cast<WINTUN_GET_RUNNING_DRIVER_VERSION_FUNC>(
        GetProcAddress(wintunDll, "WinTunGetRunningDriverVersion"));
    if (!WinTunGetRunningDriverVersion) {
        WinTunGetRunningDriverVersion = reinterpret_cast<WINTUN_GET_RUNNING_DRIVER_VERSION_FUNC>(
            GetProcAddress(wintunDll, "_WinTunGetRunningDriverVersion@0"));
    }
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