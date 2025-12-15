#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <ifdef.h>

#include "vpn_manager.h"
#include <fstream>
#include <sstream>
#include <chrono>
#include <iostream>
#include <vector>
#include <iomanip>
#include <tlhelp32.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <winreg.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace openvpn_flutter {

VPNManager::VPNManager() {
    ZeroMemory(&processInfo, sizeof(processInfo));
    wintunManager = std::make_unique<WinTunManager>();
    // Don't initialize driver in constructor - do it lazily when needed
    // This prevents crashes during plugin registration
    // initializeDriver();
}

VPNManager::~VPNManager() {
    stopVPN();
}

void VPNManager::setEventSink(flutter::EventSink<flutter::EncodableValue>* sink) {
    eventSink = sink;
}

bool VPNManager::startVPN(const std::string& config, const std::string& username, const std::string& password) {
    if (isConnected || isConnecting) {
        return false;
    }
    
    // Initialize driver if not already initialized
    if (!driverInitialized) {
        if (!initializeDriver()) {
            updateStatus("error");
            return false;
        }
        driverInitialized = true;
    }
    
    // Ensure driver is available
    if (currentDriver == DriverType::WINTUN && !isWinTunAvailable()) {
        if (allowFallbackToTAP && isTapDriverInstalled()) {
            currentDriver = DriverType::TAP_WINDOWS;
            std::cout << "Falling back to TAP-Windows driver" << std::endl;
        } else {
            updateStatus("error");
            return false;
        }
    } else if (currentDriver == DriverType::TAP_WINDOWS && !isTapDriverInstalled()) {
        updateStatus("error");
        return false;
    }
    
    // Get bundled OpenVPN executable
    std::string openVPNPath = getBundledOpenVPNPath();
    if (openVPNPath.empty()) {
        updateStatus("error");
        return false;
    }
    
    // Create config file
    if (!createConfigFile(config, username, password)) {
        updateStatus("error");
        return false;
    }
    
    try {
        // Prepare command line arguments for bundled OpenVPN
        std::ostringstream cmdStream;
        cmdStream << "\"" << openVPNPath << "\" --config \"" << currentConfigPath << "\"";
        
        // Add minimal options
        cmdStream << " --verb 3";
        
        // Disable DCO to avoid netsh permission issues
        cmdStream << " --disable-dco";
        
        // For TAP-Windows, add driver-specific options
        if (currentDriver == DriverType::TAP_WINDOWS) {
            cmdStream << " --dev-type tap";
            if (!tapAdapterName.empty()) {
                cmdStream << " --dev \"" << tapAdapterName << "\"";
            }
            std::cout << "Using TAP-Windows driver for OpenVPN connection" << std::endl;
        } else if (currentDriver == DriverType::WINTUN) {
            std::cout << "Using WinTun driver (configured via config file: dev tun + windows-driver wintun)" << std::endl;
        }
        
        std::string cmdLine = cmdStream.str();
        std::cout << "DEBUG: Full OpenVPN command line: " << cmdLine << std::endl;
        
        // DEBUG: Verify config file exists and show its first few lines
        std::ifstream configCheck(currentConfigPath);
        if (configCheck.is_open()) {
            std::cout << "DEBUG: Config file exists at: " << currentConfigPath << std::endl;
            std::string firstLine;
            int lineCount = 0;
            while (std::getline(configCheck, firstLine) && lineCount < 10) {
                std::cout << "DEBUG: Config line " << (lineCount + 1) << ": " << firstLine << std::endl;
                lineCount++;
            }
            configCheck.close();
        } else {
            std::cout << "ERROR: Config file does not exist at: " << currentConfigPath << std::endl;
        }
        
        // Check if we're already running as admin
        if (!isRunningAsAdmin()) {
            std::cout << "Application is not running as administrator. OpenVPN requires elevated privileges." << std::endl;
            updateStatus("error");
            return false;
        }
        
        // Start OpenVPN process (already elevated since app is running as admin)
        STARTUPINFOA startupInfo;
        ZeroMemory(&processInfo, sizeof(processInfo));
        ZeroMemory(&startupInfo, sizeof(startupInfo));
        startupInfo.cb = sizeof(startupInfo);
        startupInfo.dwFlags = STARTF_USESHOWWINDOW;
        startupInfo.wShowWindow = SW_HIDE;
        
        // Build command line with all arguments
        std::string fullCmdLine = "\"" + openVPNPath + "\" --config \"" + currentConfigPath + "\" --verb 3 --disable-dco";
        if (currentDriver == DriverType::TAP_WINDOWS) {
            fullCmdLine += " --dev-type tap";
            if (!tapAdapterName.empty()) {
                fullCmdLine += " --dev \"" + tapAdapterName + "\"";
            }
        }
        std::cout << "Full command line: " << fullCmdLine << std::endl;
        
        // CRITICAL: Set working directory to app directory for proper DLL loading
        // When running as admin from a shortcut, the working dir might be System32
        std::string appDir = getAppDirectory();
        std::cout << "Working directory: " << appDir << std::endl;
        
        BOOL success = CreateProcessA(
            NULL,                     // Application name
            (LPSTR)fullCmdLine.c_str(), // Command line
            NULL,                     // Process security attributes
            NULL,                     // Thread security attributes
            FALSE,                    // Inherit handles
            CREATE_NO_WINDOW,         // Creation flags - hide console window
            NULL,                     // Environment
            appDir.c_str(),          // Current directory - SET TO APP DIR!
            &startupInfo,            // Startup info
            &processInfo             // Process info
        );
        
        if (success && processInfo.hProcess) {
            hProcess = processInfo.hProcess;
            
            isConnecting = true;
            connectionStartTime = std::chrono::system_clock::now();
            
            // Reset speed tracking
            lastBytesIn = 0;
            lastBytesOut = 0;
            lastStatsTime = std::chrono::system_clock::time_point{};
            currentSpeedIn = 0.0;
            currentSpeedOut = 0.0;
            smoothedSpeedIn = 0.0;
            smoothedSpeedOut = 0.0;
            
            updateStatus("connecting");
            
            // Start monitoring thread
            shouldMonitor = true;
            statusMonitorThread = std::thread(&VPNManager::monitorConnection, this);
            
            return true;
        } else {
            DWORD error = GetLastError();
            std::cerr << "Failed to start bundled OpenVPN process. Error: " << error << std::endl;
            updateStatus("error");
            return false;
        }
    } catch (const std::exception& e) {
        std::cerr << "Exception in startVPN: " << e.what() << std::endl;
        updateStatus("error");
        return false;
    }
}

void VPNManager::stopVPN() {
    shouldMonitor = false;
    
    if (statusMonitorThread.joinable()) {
        statusMonitorThread.join();
    }
    
    if (hProcess) {
        TerminateProcess(hProcess, 0);
        WaitForSingleObject(hProcess, 5000);
        CloseHandle(hProcess);
        CloseHandle(processInfo.hThread);
        hProcess = NULL;
        ZeroMemory(&processInfo, sizeof(processInfo));
    }
    
    isConnected = false;
    isConnecting = false;
    updateStatus("disconnected");
    
    // Reset speed tracking on disconnect
    lastBytesIn = 0;
    lastBytesOut = 0;
    lastStatsTime = std::chrono::system_clock::time_point{};
    currentSpeedIn = 0.0;
    currentSpeedOut = 0.0;
    smoothedSpeedIn = 0.0;
    smoothedSpeedOut = 0.0;
    
    cleanupTempFiles();
}

std::string VPNManager::getStatus() {
    return currentStatus;
}

std::string VPNManager::getConnectionStats() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    
    // Get real network statistics from the VPN adapter
    auto [bytesIn, bytesOut] = getRealNetworkStats();
    
    // Check if we have any VPN activity (even if connection flags aren't set correctly)
    bool hasVpnActivity = (bytesIn > 0 || bytesOut > 0) || (isConnected || isConnecting);
    
    if (hasVpnActivity) {
        // Calculate connection duration
        auto connectionDuration = std::chrono::duration_cast<std::chrono::seconds>(now - connectionStartTime);
        int hours = static_cast<int>(connectionDuration.count()) / 3600;
        int minutes = (static_cast<int>(connectionDuration.count()) % 3600) / 60;
        int seconds = static_cast<int>(connectionDuration.count()) % 60;
        
        // Calculate speeds
        updateSpeedCalculations(bytesIn, bytesOut, now);
        
        // Convert bytes per second to Mbps
        double speedInMbps = (currentSpeedIn * 8.0) / (1024.0 * 1024.0);
        double speedOutMbps = (currentSpeedOut * 8.0) / (1024.0 * 1024.0);
        
        std::ostringstream oss;
        oss << "{\"connected_on\":\"";
        
        struct tm* timeinfo = std::localtime(&time_t);
        oss << std::put_time(timeinfo, "%Y-%m-%d %H:%M:%S");
        
        oss << "\",\"duration\":\"" 
            << std::setfill('0') << std::setw(2) << hours << ":"
            << std::setfill('0') << std::setw(2) << minutes << ":"
            << std::setfill('0') << std::setw(2) << seconds << "\""
            << ",\"byte_in\":\"" << bytesIn << "\""
            << ",\"byte_out\":\"" << bytesOut << "\""
            << ",\"packets_in\":\"" << bytesIn << "\""
            << ",\"packets_out\":\"" << bytesOut << "\""
            << ",\"speed_in_mbps\":\"" << std::fixed << std::setprecision(2) << speedInMbps << "\""
            << ",\"speed_out_mbps\":\"" << std::fixed << std::setprecision(2) << speedOutMbps << "\""
            << ",\"speed_in_bps\":\"" << static_cast<uint64_t>(currentSpeedIn) << "\""
            << ",\"speed_out_bps\":\"" << static_cast<uint64_t>(currentSpeedOut) << "\"}";
        
        std::cout << "🚀 Returning full stats with speeds: " << speedInMbps << " Mbps down, " << speedOutMbps << " Mbps up" << std::endl;
        return oss.str();
    }
    return "{\"connected_on\":null,\"duration\":\"00:00:00\",\"byte_in\":\"0\",\"byte_out\":\"0\",\"packets_in\":\"0\",\"packets_out\":\"0\"}";
}

bool VPNManager::initializeDriver() {
    // Try WinTun first (preferred)
    if (preferredDriver == DriverType::WINTUN || 
        (preferredDriver == DriverType::TAP_WINDOWS && allowFallbackToTAP)) {
        
        if (initializeWinTun()) {
            currentDriver = DriverType::WINTUN;
            driverInitialized = true;
            std::cout << "Using WinTun driver for VPN connections" << std::endl;
            return true;
        }
    }
    
    // Fallback to TAP-Windows if allowed
    if (allowFallbackToTAP && initializeTapDriver()) {
        currentDriver = DriverType::TAP_WINDOWS;
        driverInitialized = true;
        std::cout << "Using TAP-Windows driver for VPN connections" << std::endl;
        return true;
    }
    
    std::cerr << "Failed to initialize any VPN driver" << std::endl;
    driverInitialized = false;
    return false;
}

bool VPNManager::initializeWinTun() {
    if (!wintunManager) {
        wintunManager = std::make_unique<WinTunManager>();
    }
    
    if (!wintunManager->initialize()) {
        std::cerr << "Failed to initialize WinTun manager" << std::endl;
        return false;
    }
    
    if (!wintunManager->isWinTunAvailable()) {
        std::cerr << "WinTun is not available on this system" << std::endl;
        return false;
    }
    
    // Try using tapctl.exe first (OpenVPN 2.6.14+ preferred method)
    std::string tapctlPath = findBundledExecutable("tapctl.exe");
    if (!tapctlPath.empty()) {
        std::cout << "Found tapctl.exe, attempting to create WinTun adapter using tapctl..." << std::endl;
        std::string adapterName = "OpenVPN-Flutter";
        
        // Run: tapctl.exe create --hwid wintun --name "OpenVPN-Flutter"
        std::ostringstream cmdStream;
        cmdStream << "\"" << tapctlPath << "\" create --hwid wintun --name \"" << adapterName << "\"";
        std::string cmdLine = cmdStream.str();
        std::cout << "Running: " << cmdLine << std::endl;
        
        // CRITICAL: Set working directory to app directory for proper DLL loading
        std::string appDir = getAppDirectory();
        std::cout << "Working directory for tapctl: " << appDir << std::endl;
        
        STARTUPINFOA startupInfo;
        PROCESS_INFORMATION tapctlProcessInfo;
        ZeroMemory(&tapctlProcessInfo, sizeof(tapctlProcessInfo));
        ZeroMemory(&startupInfo, sizeof(startupInfo));
        startupInfo.cb = sizeof(startupInfo);
        startupInfo.dwFlags = STARTF_USESHOWWINDOW;
        startupInfo.wShowWindow = SW_HIDE;
        
        BOOL success = CreateProcessA(
            NULL,
            (LPSTR)cmdLine.c_str(),
            NULL,
            NULL,
            FALSE,
            CREATE_NO_WINDOW,
            NULL,
            appDir.c_str(),          // Set working directory to app dir
            &startupInfo,
            &tapctlProcessInfo
        );
        
        if (success) {
            WaitForSingleObject(tapctlProcessInfo.hProcess, 5000); // Wait up to 5 seconds
            DWORD exitCode;
            if (GetExitCodeProcess(tapctlProcessInfo.hProcess, &exitCode)) {
                CloseHandle(tapctlProcessInfo.hProcess);
                CloseHandle(tapctlProcessInfo.hThread);
                
                if (exitCode == 0) {
                    std::cout << "Successfully created WinTun adapter using tapctl.exe" << std::endl;
                    std::cout << "WinTun driver initialized successfully" << std::endl;
                    return true;
                } else {
                    std::cout << "tapctl.exe exited with code: " << exitCode << ", falling back to programmatic creation" << std::endl;
                }
            } else {
                CloseHandle(tapctlProcessInfo.hProcess);
                CloseHandle(tapctlProcessInfo.hThread);
                std::cout << "Could not get tapctl.exe exit code, falling back to programmatic creation" << std::endl;
            }
        } else {
            DWORD error = GetLastError();
            std::cout << "Failed to run tapctl.exe (error " << error << "), falling back to programmatic creation" << std::endl;
        }
    } else {
        std::cout << "tapctl.exe not found, using programmatic adapter creation" << std::endl;
    }
    
    // Fallback: Create WinTun adapter programmatically
    if (!wintunManager->createAdapter("OpenVPN-Flutter")) {
        std::cerr << "Failed to create WinTun adapter programmatically" << std::endl;
        return false;
    }
    
    std::cout << "WinTun driver initialized successfully (programmatic creation)" << std::endl;
    return true;
}

bool VPNManager::initializeTapDriver() {
    // Check if TAP driver is installed
    tapDriverInstalled = isTapDriverInstalled();
    std::cout << "TAP driver installed: " << (tapDriverInstalled ? "Yes" : "No") << std::endl;
    
    if (tapDriverInstalled) {
        // Find existing TAP adapter
        tapAdapterName = findTapAdapter();
        std::cout << "TAP adapter found: " << (tapAdapterName.empty() ? "None" : tapAdapterName) << std::endl;
        
        if (tapAdapterName.empty()) {
            // Try to create one
            std::cout << "Attempting to create TAP adapter..." << std::endl;
            return createTapAdapter();
        }
        return true;
    }
    
    // Try to install TAP driver
    std::cout << "Attempting to install TAP driver..." << std::endl;
    return installTapDriver();
}

bool VPNManager::installTapDriver() {
    try {
        std::string tapDriverPath = findBundledExecutable("tapinstall.exe");
        if (tapDriverPath.empty()) {
            std::cerr << "TAP driver installer not found in bundle" << std::endl;
            return false;
        }
        
        std::string tapInfPath = findBundledExecutable("OemVista.inf");
        if (tapInfPath.empty()) {
            std::cerr << "TAP driver INF file not found in bundle" << std::endl;
            return false;
        }
        
        // Install TAP driver (requires admin privileges)
        std::string installCmd = "\"" + tapDriverPath + "\" install \"" + tapInfPath + "\" tap0901";
        
        if (!runAsAdmin(installCmd)) {
            std::cerr << "Failed to install TAP driver" << std::endl;
            return false;
        }
        
        // Create TAP adapter
        std::string createCmd = "\"" + tapDriverPath + "\" create tap0901 TAPVPN";
        if (!runAsAdmin(createCmd)) {
            std::cerr << "Failed to create TAP adapter" << std::endl;
            return false;
        }
        
        tapDriverInstalled = true;
        tapAdapterName = findTapAdapter();
        
        return !tapAdapterName.empty();
    } catch (...) {
        return false;
    }
}

bool VPNManager::isTapDriverInstalled() {
    // Check registry for TAP driver
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, 
                     "SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002BE10318}", 
                     0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        
        DWORD index = 0;
        char subKeyName[256];
        DWORD subKeyNameSize = sizeof(subKeyName);
        
        while (RegEnumKeyExA(hKey, index++, subKeyName, &subKeyNameSize, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
            HKEY hSubKey;
            if (RegOpenKeyExA(hKey, subKeyName, 0, KEY_READ, &hSubKey) == ERROR_SUCCESS) {
                char driverDesc[256];
                DWORD driverDescSize = sizeof(driverDesc);
                
                if (RegQueryValueExA(hSubKey, "DriverDesc", NULL, NULL, (LPBYTE)driverDesc, &driverDescSize) == ERROR_SUCCESS) {
                    if (strstr(driverDesc, "TAP-Windows") != NULL || strstr(driverDesc, "TAP-Win32") != NULL) {
                        RegCloseKey(hSubKey);
                        RegCloseKey(hKey);
                        return true;
                    }
                }
                RegCloseKey(hSubKey);
            }
            subKeyNameSize = sizeof(subKeyName);
        }
        RegCloseKey(hKey);
    }
    return false;
}

std::string VPNManager::getBundledOpenVPNPath() {
    // Look for bundled OpenVPN executable in multiple possible locations
    std::string appDir = getAppDirectory();
    
    std::vector<std::string> possiblePaths = {
        // Installed app locations (most common for installed apps)
        appDir + "\\openvpn_bundle\\bin\\openvpn.exe",
        appDir + "\\bin\\openvpn.exe",
        appDir + "\\openvpn.exe",
        // Flutter build output locations
        appDir + "\\data\\flutter_assets\\windows\\bin\\openvpn.exe",
        appDir + "\\data\\flutter_assets\\bin\\openvpn.exe",
        // Direct bundle locations
        appDir + "\\openvpn\\openvpn.exe",
        // Plugin asset locations  
        appDir + "\\..\\..\\..\\windows\\runner\\bin\\openvpn.exe",
        // Development locations
        ".\\bin\\openvpn.exe",
        ".\\openvpn.exe"
    };
    
    for (const auto& path : possiblePaths) {
        if (PathFileExistsA(path.c_str())) {
            std::cout << "Found OpenVPN executable at: " << path << std::endl;
            return path;
        }
    }
    
    std::cerr << "OpenVPN executable not found in any expected location" << std::endl;
    std::cerr << "Searched in app directory: " << appDir << std::endl;
    return "";
}

std::string VPNManager::findBundledExecutable(const std::string& filename) {
    std::string appDir = getAppDirectory();
    
    std::vector<std::string> possiblePaths = {
        // Flutter build output locations
        appDir + "\\data\\flutter_assets\\windows\\bin\\" + filename,
        appDir + "\\data\\flutter_assets\\bin\\" + filename,
        appDir + "\\data\\flutter_assets\\drivers\\" + filename,
        // Direct bundle locations
        appDir + "\\bin\\" + filename,
        appDir + "\\drivers\\" + filename,
        appDir + "\\openvpn_bundle\\bin\\" + filename,
        appDir + "\\openvpn_bundle\\" + filename,
        appDir + "\\" + filename,
        // Plugin asset locations
        appDir + "\\..\\..\\..\\windows\\runner\\bin\\" + filename,
        appDir + "\\..\\..\\..\\windows\\runner\\drivers\\" + filename,
        // Development locations
        ".\\bin\\" + filename,
        ".\\drivers\\" + filename,
        ".\\" + filename
    };
    
    for (const auto& path : possiblePaths) {
        if (PathFileExistsA(path.c_str())) {
            std::cout << "Found " << filename << " at: " << path << std::endl;
            return path;
        }
    }
    
    std::cout << "Warning: " << filename << " not found in any expected location" << std::endl;
    return "";
}

void VPNManager::monitorConnection() {
    int connectionAttempts = 0;
    const int maxConnectionAttempts = 300; // 30 seconds
    int connectedStableCount = 0;
    const int requiredStableCount = 10; // 1 second of stable connection
    
    while (shouldMonitor && hProcess) {
        DWORD exitCode;
        if (GetExitCodeProcess(hProcess, &exitCode)) {
            if (exitCode == STILL_ACTIVE) {
                if (isConnecting) {
                    connectionAttempts++;
                    
                    // Check if OpenVPN process is running and stable
                    if (connectionAttempts > 50) { // Give 5 seconds for process to start
                        // Try to detect actual connection by checking for network adapter changes
                        // or by reading OpenVPN log output (simplified check here)
                        bool connectionDetected = checkConnectionStatus();
                        
                        if (connectionDetected) {
                            connectedStableCount++;
                            if (connectedStableCount >= requiredStableCount) {
                                isConnecting = false;
                                isConnected = true;
                                updateStatusThreadSafe("connected");
                                std::cout << "VPN connection established successfully" << std::endl;
                            }
                        } else {
                            connectedStableCount = 0; // Reset counter if connection not stable
                        }
                    }
                    
                    if (connectionAttempts > maxConnectionAttempts) {
                        updateStatusThreadSafe("error");
                        std::cerr << "VPN connection timeout" << std::endl;
                        break;
                    }
                } else if (isConnected) {
                    // Continuously monitor active connection
                    if (!checkConnectionStatus()) {
                        // Connection lost
                        isConnected = false;
                        updateStatusThreadSafe("disconnected");
                        std::cout << "VPN connection lost" << std::endl;
                        break;
                    }
                }
            } else {
                // Process exited
                isConnected = false;
                isConnecting = false;
                updateStatusThreadSafe("disconnected");
                std::cout << "OpenVPN process exited with code: " << exitCode << std::endl;
                break;
            }
        } else {
            // Error getting process status
            isConnected = false;
            isConnecting = false;
            updateStatusThreadSafe("error");
            std::cerr << "Error monitoring OpenVPN process" << std::endl;
            break;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

bool VPNManager::checkConnectionStatus() {
    // Simple heuristic: check if we have a VPN adapter with an IP address
    // This is a basic implementation - in production you might want to:
    // 1. Parse OpenVPN management interface
    // 2. Check for specific network routes
    // 3. Ping VPN gateway
    
    if (currentDriver == DriverType::WINTUN) {
        // For WinTun, check if adapter has IP assignment
        return wintunManager && !wintunManager->getAdapterName().empty();
    } else if (currentDriver == DriverType::TAP_WINDOWS) {
        // For TAP, check if adapter is up and has IP
        return !tapAdapterName.empty() && checkTapAdapterStatus();
    }
    
    return false;
}

bool VPNManager::checkTapAdapterStatus() {
    if (tapAdapterName.empty()) return false;
    
    // Check if TAP adapter has an IP address assigned
    ULONG bufferSize = 0;
    GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, NULL, NULL, &bufferSize);
    
    if (bufferSize > 0) {
        std::vector<char> buffer(bufferSize);
        PIP_ADAPTER_ADDRESSES adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        
        if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, NULL, adapters, &bufferSize) == NO_ERROR) {
            for (PIP_ADAPTER_ADDRESSES adapter = adapters; adapter != NULL; adapter = adapter->Next) {
                if (adapter->AdapterName && 
                    std::string(adapter->AdapterName) == tapAdapterName &&
                    adapter->OperStatus == IfOperStatusUp &&
                    adapter->FirstUnicastAddress != NULL) {
                    return true; // Adapter is up and has IP address
                }
            }
        }
    }
    
    return false;
}

void VPNManager::updateStatus(const std::string& status) {
    // This method should only be called from the main thread
    currentStatus = status;
    if (eventSink) {
        eventSink->Success(flutter::EncodableValue(status));
    }
}

void VPNManager::updateStatusThreadSafe(const std::string& status) {
    // Thread-safe method for background threads to queue status updates
    std::lock_guard<std::mutex> lock(statusMutex);
    pendingStatusUpdates.push(status);
    std::cout << "Queued status update: " << status << " (from background thread)" << std::endl;
}

void VPNManager::processPendingStatusUpdates() {
    // Process all pending status updates from the main thread
    std::lock_guard<std::mutex> lock(statusMutex);
    while (!pendingStatusUpdates.empty()) {
        std::string status = pendingStatusUpdates.front();
        pendingStatusUpdates.pop();
        
        // Update status using the main thread
        currentStatus = status;
        if (eventSink) {
            eventSink->Success(flutter::EncodableValue(status));
            std::cout << "Processed status update from main thread: " << status << std::endl;
        }
    }
}

bool VPNManager::createConfigFile(const std::string& config, const std::string& username, const std::string& password) {
    try {
        // Use app directory instead of user temp to ensure elevated process can access it
        std::string appDir = getAppDirectory();
        currentConfigPath = appDir + "\\openvpn_flutter_config.ovpn";
        
        std::cout << "Creating config file at: " << currentConfigPath << std::endl;
        
        std::ofstream configFile(currentConfigPath);
        if (!configFile.is_open()) {
            return false;
        }
        
        // Modify config based on driver type
        std::string modifiedConfig = config;
        
        // Remove deprecated client-cert-not-required option if present
        size_t pos = 0;
        while ((pos = modifiedConfig.find("client-cert-not-required", pos)) != std::string::npos) {
            size_t endPos = modifiedConfig.find('\n', pos);
            if (endPos == std::string::npos) endPos = modifiedConfig.length();
            modifiedConfig.erase(pos, endPos - pos + 1);
        }
        
        // When using WinTun, remove dev/dev-type directives from config file
        // We specify them on the command line instead to avoid conflicts
        if (currentDriver == DriverType::WINTUN) {
            // Remove 'dev tap' lines
            pos = 0;
            while ((pos = modifiedConfig.find("dev tap", pos)) != std::string::npos) {
                size_t lineStart = modifiedConfig.rfind('\n', pos);
                if (lineStart == std::string::npos) lineStart = 0;
                else lineStart++;
                
                size_t lineEnd = modifiedConfig.find('\n', pos);
                if (lineEnd == std::string::npos) lineEnd = modifiedConfig.length();
                else lineEnd++;
                
                std::string line = modifiedConfig.substr(lineStart, lineEnd - lineStart);
                if (line.find("dev tap") != std::string::npos && 
                    line.find('#') == std::string::npos) {
                    modifiedConfig.erase(lineStart, lineEnd - lineStart);
                    pos = lineStart;
                    std::cout << "Removed 'dev tap' line for WinTun" << std::endl;
                } else {
                    pos = lineEnd;
                }
            }
            
            // Remove any existing windows-driver directives first (we'll add the correct one)
            pos = 0;
            while ((pos = modifiedConfig.find("windows-driver", pos)) != std::string::npos) {
                size_t lineStart = modifiedConfig.rfind('\n', pos);
                if (lineStart == std::string::npos) lineStart = 0;
                else lineStart++;
                
                size_t lineEnd = modifiedConfig.find('\n', pos);
                if (lineEnd == std::string::npos) lineEnd = modifiedConfig.length();
                else lineEnd++;
                
                std::string line = modifiedConfig.substr(lineStart, lineEnd - lineStart);
                if (line.find("windows-driver") != std::string::npos && 
                    line.find('#') == std::string::npos) {
                    modifiedConfig.erase(lineStart, lineEnd - lineStart);
                    pos = lineStart;
                    std::cout << "Removed existing 'windows-driver' line from config (will add correct one)" << std::endl;
                } else {
                    pos = lineEnd;
                }
            }
            
            // Add 'windows-driver wintun' - REQUIRED for OpenVPN 2.6.14+ WinTun
            // This directive tells OpenVPN to use WinTun driver on Windows
            // Note: 'dev tun' is already in the config file from the API
            if (modifiedConfig.find("windows-driver wintun") == std::string::npos) {
                // Add after 'dev tun' line (which should already exist)
                size_t devTunPos = modifiedConfig.find("dev tun");
                if (devTunPos != std::string::npos) {
                    size_t devTunLineEnd = modifiedConfig.find('\n', devTunPos);
                    if (devTunLineEnd != std::string::npos) {
                        modifiedConfig.insert(devTunLineEnd + 1, "windows-driver wintun\n");
                        std::cout << "Added 'windows-driver wintun' to config file (REQUIRED for WinTun)" << std::endl;
                    }
                } else {
                    // Fallback: if dev tun not found, add after remote
                    size_t insertPos = modifiedConfig.find("remote ");
                    if (insertPos != std::string::npos) {
                        size_t remoteLineEnd = modifiedConfig.find('\n', insertPos);
                        if (remoteLineEnd != std::string::npos) {
                            modifiedConfig.insert(remoteLineEnd + 1, "windows-driver wintun\n");
                            std::cout << "Added 'windows-driver wintun' to config file (REQUIRED for WinTun)" << std::endl;
                        }
                    }
                }
            } else {
                std::cout << "Found 'windows-driver wintun' in config - keeping it" << std::endl;
            }
            
            // Remove 'persist-tun' as it may conflict with WinTun
            pos = 0;
            while ((pos = modifiedConfig.find("persist-tun", pos)) != std::string::npos) {
                size_t lineStart = modifiedConfig.rfind('\n', pos);
                if (lineStart == std::string::npos) lineStart = 0;
                else lineStart++;
                
                size_t lineEnd = modifiedConfig.find('\n', pos);
                if (lineEnd == std::string::npos) lineEnd = modifiedConfig.length();
                else lineEnd++;
                
                std::string line = modifiedConfig.substr(lineStart, lineEnd - lineStart);
                if (line.find("persist-tun") != std::string::npos && 
                    line.find('#') == std::string::npos) {
                    modifiedConfig.erase(lineStart, lineEnd - lineStart);
                    pos = lineStart;
                    std::cout << "Removed 'persist-tun' line for WinTun compatibility" << std::endl;
                } else {
                    pos = lineEnd;
                }
            }
            
            // Remove any existing dev-type directives (we specify on command line)
            pos = 0;
            while ((pos = modifiedConfig.find("dev-type", pos)) != std::string::npos) {
                size_t lineStart = modifiedConfig.rfind('\n', pos);
                if (lineStart == std::string::npos) lineStart = 0;
                else lineStart++;
                
                size_t lineEnd = modifiedConfig.find('\n', pos);
                if (lineEnd == std::string::npos) lineEnd = modifiedConfig.length();
                else lineEnd++;
                
                std::string line = modifiedConfig.substr(lineStart, lineEnd - lineStart);
                if (line.find("dev-type") != std::string::npos && 
                    line.find('#') == std::string::npos) {
                    modifiedConfig.erase(lineStart, lineEnd - lineStart);
                    pos = lineStart;
                    std::cout << "Removed 'dev-type' line from config (using command line instead)" << std::endl;
                } else {
                    pos = lineEnd;
                }
            }
        }
        
        configFile << modifiedConfig;
        
        if (currentDriver == DriverType::WINTUN) {
            std::cout << "Modified config for WinTun compatibility:" << std::endl;
            std::cout << "  - Config file: 'dev tun' (from API) + 'windows-driver wintun' (added)" << std::endl;
            std::cout << "  - Command line: Basic options only (WinTun configured via config file)" << std::endl;
        } else {
            std::cout << "Using original config from API without modifications" << std::endl;
        }
        
        // DEBUG: Show full config content for WinTun
        if (currentDriver == DriverType::WINTUN) {
            std::cout << "DEBUG: Full modified config content:" << std::endl;
            std::cout << modifiedConfig << std::endl;
            std::cout << "DEBUG: End of modified config" << std::endl;
        } else {
            std::cout << "Config preview (first 500 chars): " << modifiedConfig.substr(0, 500) << std::endl;
        }
        
        if (!username.empty() && !password.empty()) {
            std::string authPath = appDir + "\\openvpn_flutter_auth.txt";
            
            std::ofstream authFile(authPath);
            if (authFile.is_open()) {
                authFile << username << "\n" << password;
                authFile.close();
                
                configFile << "\nauth-user-pass \"" << authPath << "\"";
            }
        }
        
        configFile.close();
        return true;
    } catch (...) {
        return false;
    }
}

void VPNManager::cleanupTempFiles() {
    if (!currentConfigPath.empty()) {
        DeleteFileA(currentConfigPath.c_str());
        
        std::string authPath = currentConfigPath;
        size_t pos = authPath.find_last_of('.');
        if (pos != std::string::npos) {
            authPath = authPath.substr(0, pos) + "_auth.txt";
            DeleteFileA(authPath.c_str());
        }
        
        currentConfigPath.clear();
    }
}

std::string VPNManager::findTapAdapter() {
    // Enumerate network adapters to find TAP adapter
    ULONG bufferSize = 0;
    GetAdaptersAddresses(AF_UNSPEC, 0, NULL, NULL, &bufferSize);
    
    if (bufferSize > 0) {
        std::vector<char> buffer(bufferSize);
        PIP_ADAPTER_ADDRESSES adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        
        if (GetAdaptersAddresses(AF_UNSPEC, 0, NULL, adapters, &bufferSize) == NO_ERROR) {
            for (PIP_ADAPTER_ADDRESSES adapter = adapters; adapter != NULL; adapter = adapter->Next) {
                if (adapter->Description && 
                    (wcsstr(adapter->Description, L"TAP-Windows") != NULL || 
                     wcsstr(adapter->Description, L"TAP-Win32") != NULL)) {
                    // AdapterName is already a narrow string (PCHAR)
                    return std::string(adapter->AdapterName);
                }
            }
        }
    }
    
    return "";
}

bool VPNManager::createTapAdapter() {
    std::string tapInstallPath = findBundledExecutable("tapinstall.exe");
    if (tapInstallPath.empty()) {
        return false;
    }
    
    std::string createCmd = "\"" + tapInstallPath + "\" create tap0901 OpenVPN_TAP";
    return runAsAdmin(createCmd);
}

bool VPNManager::enableTapAdapter() {
    if (tapAdapterName.empty()) return false;
    
    std::string cmd = "netsh interface set interface \"" + tapAdapterName + "\" admin=enable";
    return runAsAdmin(cmd);
}

bool VPNManager::disableTapAdapter() {
    if (tapAdapterName.empty()) return false;
    
    std::string cmd = "netsh interface set interface \"" + tapAdapterName + "\" admin=disable";
    return runAsAdmin(cmd);
}

bool VPNManager::runAsAdmin(const std::string& command, const std::string& params) {
    SHELLEXECUTEINFOA sei = { 0 };
    sei.cbSize = sizeof(sei);
    sei.lpVerb = "runas";
    sei.lpFile = "cmd.exe";
    
    std::string fullCmd = "/c " + command + " " + params;
    sei.lpParameters = fullCmd.c_str();
    sei.nShow = SW_HIDE;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    
    if (ShellExecuteExA(&sei)) {
        if (sei.hProcess) {
            WaitForSingleObject(sei.hProcess, 30000); // 30 second timeout
            CloseHandle(sei.hProcess);
            return true;
        }
    }
    return false;
}

bool VPNManager::isRunningAsAdmin() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = NULL;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    
    if (AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(NULL, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    
    return isAdmin == TRUE;
}

std::string VPNManager::getAppDirectory() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    PathRemoveFileSpecA(path);
    return std::string(path);
}

bool VPNManager::isWinTunAvailable() {
    if (!wintunManager) {
        wintunManager = std::make_unique<WinTunManager>();
    }
    return wintunManager->isWinTunAvailable();
}

DriverType VPNManager::getCurrentDriver() const {
    return currentDriver;
}

void VPNManager::setPreferredDriver(DriverType type, bool allowFallback) {
    preferredDriver = type;
    allowFallbackToTAP = allowFallback;
}

std::pair<uint64_t, uint64_t> VPNManager::getRealNetworkStats() {
    uint64_t bytesIn = 0;
    uint64_t bytesOut = 0;
    
    // Get network adapter statistics
    ULONG bufferSize = 0;
    GetAdaptersAddresses(AF_UNSPEC, 0, NULL, NULL, &bufferSize);
    
    if (bufferSize > 0) {
        std::vector<char> buffer(bufferSize);
        PIP_ADAPTER_ADDRESSES adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        
        if (GetAdaptersAddresses(AF_UNSPEC, 0, NULL, adapters, &bufferSize) == NO_ERROR) {
            for (PIP_ADAPTER_ADDRESSES adapter = adapters; adapter != NULL; adapter = adapter->Next) {
                // Look for VPN-related adapters
                if (adapter->Description && 
                    (wcsstr(adapter->Description, L"TAP-Windows") != NULL ||
                     wcsstr(adapter->Description, L"OpenVPN") != NULL ||
                     wcsstr(adapter->Description, L"WinTun") != NULL ||
                     wcsstr(adapter->Description, L"Wintun") != NULL ||
                     wcsstr(adapter->Description, L"Data Channel Offload") != NULL)) {
                    
                    // Get interface statistics using GetIfEntry2
                    MIB_IF_ROW2 ifRow;
                    ZeroMemory(&ifRow, sizeof(ifRow));
                    ifRow.InterfaceIndex = adapter->IfIndex;
                    
                    if (GetIfEntry2(&ifRow) == NO_ERROR) {
                        bytesIn += ifRow.InOctets;
                        bytesOut += ifRow.OutOctets;
                        
                        std::wcout << L"Found VPN adapter: " << adapter->Description 
                                  << L" - In: " << ifRow.InOctets 
                                  << L" Out: " << ifRow.OutOctets << std::endl;
                    }
                }
            }
        }
    }
    
    return std::make_pair(bytesIn, bytesOut);
}

void VPNManager::updateSpeedCalculations(uint64_t bytesIn, uint64_t bytesOut, const std::chrono::system_clock::time_point& now) {
    // Initialize on first call
    if (lastStatsTime.time_since_epoch().count() == 0) {
        lastBytesIn = bytesIn;
        lastBytesOut = bytesOut;
        lastStatsTime = now;
        currentSpeedIn = 0.0;
        currentSpeedOut = 0.0;
        smoothedSpeedIn = 0.0;
        smoothedSpeedOut = 0.0;
        return;
    }
    
    // Calculate time difference
    auto timeDiff = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastStatsTime);
    double timeDiffSeconds = timeDiff.count() / 1000.0;
    
    // Only calculate if enough time has passed (at least 100ms for accurate speed calculation)
    // This prevents division by very small numbers and reduces jitter
    if (timeDiffSeconds >= 0.1) {
        // Calculate byte differences
        uint64_t byteInDiff = bytesIn - lastBytesIn;
        uint64_t byteOutDiff = bytesOut - lastBytesOut;
        
        // Calculate raw speeds (bytes per second)
        double rawSpeedIn = byteInDiff / timeDiffSeconds;
        double rawSpeedOut = byteOutDiff / timeDiffSeconds;
        
        // Smooth the speeds with exponential moving average to reduce jitter
        // Use member variables instead of static to reset properly on disconnect
        // Weight: 70% new value, 30% old value for responsive but stable speeds
        smoothedSpeedIn = (smoothedSpeedIn * 0.3) + (rawSpeedIn * 0.7);
        smoothedSpeedOut = (smoothedSpeedOut * 0.3) + (rawSpeedOut * 0.7);
        
        currentSpeedIn = smoothedSpeedIn;
        currentSpeedOut = smoothedSpeedOut;
        
        // Update tracking variables
        lastBytesIn = bytesIn;
        lastBytesOut = bytesOut;
        lastStatsTime = now;
    }
}

} // namespace openvpn_flutter 