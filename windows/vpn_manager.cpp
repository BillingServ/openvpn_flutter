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
#include <objbase.h>
#include <algorithm>
#include <cstring>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")

namespace openvpn_flutter {

namespace {
std::string guidString(const GUID& guid) {
    wchar_t text[40]{};
    if (!StringFromGUID2(guid, text, 40)) return "";
    const std::wstring value(text);
    return std::string(value.begin(), value.end());
}

std::string newAdapterAlias() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return "";
    const auto value = guidString(guid);
    return value.empty() ? "" : "OpenVPN-Flutter-" + value.substr(1, value.size() - 2);
}
} // namespace

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
    // CRITICAL: Clear any pending status updates from previous connection
    // This prevents stale "disconnected" updates from overriding the new "connecting" status
    {
        std::lock_guard<std::mutex> lock(statusMutex);
        while (!pendingStatusUpdates.empty()) {
            std::cout << "Clearing stale pending status: " << pendingStatusUpdates.front() << std::endl;
            pendingStatusUpdates.pop();
        }
    }
    
    if (isConnected || isConnecting) {
        std::cout << "startVPN: Already connected or connecting, returning false" << std::endl;
        return false;
    }
    
    // The previous monitor can have exited after process/adapter loss while its
    // std::thread remains joinable. Join and clean up outside statusMutex before
    // creating the next process and monitor.
    if (statusMonitorThread.joinable() || hProcess) stopVPN();
    if (hProcess) return false;

    if (!isRunningAsAdmin()) {
        updateStatus("error");
        return false;
    }
    if (!driverInitialized && !initializeDriver()) {
        updateStatus("error");
        return false;
    }
    if (!hasOwnedAdapter) {
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
        // --dev-node selects this exact adapter GUID for both drivers. Appended
        // managed options also keep the fresh log's event format deterministic.
        std::string fullCmdLine = "\"" + openVPNPath + "\" --config \"" + currentConfigPath +
            "\" --dev " + deviceType + " --dev-type " + deviceType +
            " --windows-driver " + (currentDriver == DriverType::WINTUN ? "wintun" : "tap-windows6") +
            " --dev-node \"" + ownedAdapterNode + "\" --disable-dco --verb 3 --mute 0" +
            " --suppress-timestamps --log-append \"" + currentLogPath + "\"";

        STARTUPINFOA startupInfo{};
        ZeroMemory(&processInfo, sizeof(processInfo));
        startupInfo.cb = sizeof(startupInfo);
        startupInfo.dwFlags = STARTF_USESHOWWINDOW;
        startupInfo.wShowWindow = SW_HIDE;

        // CRITICAL: Set working directory to app directory for proper DLL loading
        // When running as admin from a shortcut, the working dir might be System32
        std::string appDir = getAppDirectory();
        std::cout << "Working directory: " << appDir << std::endl;
        
        BOOL success = CreateProcessA(
            openVPNPath.c_str(),      // Exact bundled executable
            fullCmdLine.data(),        // Mutable command line (CreateProcess may modify it)
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
    std::cout << "stopVPN: Starting disconnect process..." << std::endl;
    
    // Signal the monitor thread to stop
    shouldMonitor = false;
    
    // Wait for the monitoring thread to finish
    if (statusMonitorThread.joinable()) {
        std::cout << "stopVPN: Waiting for monitor thread to finish..." << std::endl;
        statusMonitorThread.join();
        std::cout << "stopVPN: Monitor thread finished" << std::endl;
    }
    
    // Terminate OpenVPN process if running
    if (hProcess) {
        std::cout << "stopVPN: Terminating OpenVPN process..." << std::endl;
        TerminateProcess(hProcess, 0);
        if (WaitForSingleObject(hProcess, 5000) != WAIT_OBJECT_0) {
            updateStatus("error");
            return;
        }
        CloseHandle(hProcess);
        CloseHandle(processInfo.hThread);
        hProcess = NULL;
        ZeroMemory(&processInfo, sizeof(processInfo));
        std::cout << "stopVPN: OpenVPN process terminated" << std::endl;
    }
    
    // CRITICAL: Clear all connection state flags
    isConnected = false;
    isConnecting = false;
    
    // CRITICAL: Reset driver initialization flag so it will be re-initialized on next connect
    // This ensures a fresh WinTun adapter is created, avoiding conflicts with WireGuard
    driverInitialized = false;
    
    // CRITICAL: Clear any pending status updates from the monitor thread
    // These might contain stale "disconnected" or "connecting" states
    {
        std::lock_guard<std::mutex> lock(statusMutex);
        while (!pendingStatusUpdates.empty()) {
            std::cout << "stopVPN: Clearing pending status: " << pendingStatusUpdates.front() << std::endl;
            pendingStatusUpdates.pop();
        }
    }
    
    // Now send the final disconnected status
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
    releaseOwnedAdapter();
    std::cout << "stopVPN: Disconnect complete, ready for new connection" << std::endl;
}

std::string VPNManager::getStatus() {
    std::string status;
    {
        std::lock_guard<std::mutex> lock(statusMutex);
        status = currentStatus;
    }
    // A timed-out start or failed stop can retain a live child. "error" is a
    // terminal-down stage in Dart, so preserve establishing until child exit
    // is confirmed. An unreadable process handle is also unknown, never down.
    if (hProcess) {
        DWORD exitCode = 0;
        if (!GetExitCodeProcess(hProcess, &exitCode)) return "connecting";
        if (exitCode != STILL_ACTIVE) return "disconnected";
        return status == "connected" && checkConnectionStatus() ? "connected" : "connecting";
    }
    return status == "connected" || status == "connecting" ? "disconnected" : status;
}

std::string VPNManager::getConnectionStats() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    
    // Get real network statistics from the VPN adapter
    auto [bytesIn, bytesOut] = getRealNetworkStats();
    
    // Check if we have any VPN activity (even if connection flags aren't set correctly)
    bool hasVpnActivity = checkConnectionStatus();
    
    if (hasVpnActivity) {
        uint64_t packetsIn = 0, packetsOut = 0;
        {
            std::lock_guard<std::mutex> lock(connectionMutex);
            MIB_IF_ROW2 row{};
            if (readOwnedAdapterRow(row)) {
                packetsIn = row.InUcastPkts + row.InNUcastPkts;
                packetsOut = row.OutUcastPkts + row.OutNUcastPkts;
            }
        }
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
            << ",\"packets_in\":\"" << packetsIn << "\""
            << ",\"packets_out\":\"" << packetsOut << "\""
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
    if (driverInitialized && hasOwnedAdapter) return true;
    if (hProcess) return false;
    releaseOwnedAdapter();
    if (preferredDriver == DriverType::WINTUN && initializeWinTun()) {
        currentDriver = DriverType::WINTUN;
        driverInitialized = true;
        return true;
    }
    if ((preferredDriver == DriverType::TAP_WINDOWS || allowFallbackToTAP) && initializeTapDriver()) {
        currentDriver = DriverType::TAP_WINDOWS;
        driverInitialized = true;
        return true;
    }
    driverInitialized = false;
    return false;
}

bool VPNManager::initializeWinTun() {
    if (!wintunManager) wintunManager = std::make_unique<WinTunManager>();
    if (!wintunManager->initialize()) return false;
    if (createOwnedAdapter("wintun")) return true;

    // The DLL fallback also creates a unique adapter, held until the child exits.
    const auto alias = newAdapterAlias();
    if (alias.empty() || !wintunManager->createAdapter(alias)) return false;
    if (captureOwnedAdapter(alias)) return true;
    wintunManager->destroyAdapter();
    return false;
}

bool VPNManager::initializeTapDriver() {
    tapDriverInstalled = isTapDriverInstalled();
    if (!tapDriverInstalled) return installTapDriver();
    return createTapAdapter();
}

bool VPNManager::installTapDriver() {
    const auto installer = findBundledExecutable("tapinstall.exe");
    const auto inf = findBundledExecutable("OemVista.inf");
    if (installer.empty() || inf.empty()) return false;
    if (!runAsAdmin("\"" + installer + "\" install \"" + inf + "\" tap0901")) return false;
    tapDriverInstalled = isTapDriverInstalled();
    return tapDriverInstalled && createTapAdapter();
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
    const int maxConnectionAttempts = 300;
    while (shouldMonitor && hProcess) {
        DWORD exitCode = 0;
        if (!GetExitCodeProcess(hProcess, &exitCode) || exitCode != STILL_ACTIVE) {
            isConnected = false;
            isConnecting = false;
            updateStatusThreadSafe("disconnected");
            break;
        }
        if (checkConnectionStatus()) {
            connectionAttempts = 0;
            if (!isConnected.exchange(true)) {
                isConnecting = false;
                updateStatusThreadSafe("connected");
            }
        } else {
            if (isConnected.exchange(false)) {
                isConnecting = true;
                connectionAttempts = 0;
                updateStatusThreadSafe("connecting");
            }
            if (++connectionAttempts > maxConnectionAttempts) {
                isConnecting = false;
                updateStatusThreadSafe("error");
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

bool VPNManager::checkConnectionStatus() {
    std::lock_guard<std::mutex> lock(connectionMutex);
    DWORD exitCode = 0;
    if (!hProcess || !GetExitCodeProcess(hProcess, &exitCode) || exitCode != STILL_ACTIVE) return false;
    if (!readConnectionLog()) return false;
    MIB_IF_ROW2 row{};
    return readOwnedAdapterRow(row);
}

bool VPNManager::readConnectionLog() {
    // A precreated adapter and a running process cannot prove a TLS/session
    // handshake. Read only complete events from this launch's fresh log.
    std::ifstream log(currentLogPath, std::ios::binary);
    if (!log) return false;
    log.seekg(0, std::ios::end);
    const std::streamoff size = log.tellg();
    if (size < 0) return false;
    if (size < logReadOffset) {
        logReadOffset = 0;
        pendingLogLine.clear();
        initializationComplete = false;
        initializationFailed = false;
    }
    log.seekg(logReadOffset);
    char chunk[65536];
    log.read(chunk, sizeof(chunk));
    const auto count = log.gcount();
    logReadOffset += count;
    pendingLogLine.append(chunk, static_cast<size_t>(count));
    size_t end;
    while ((end = pendingLogLine.find('\n')) != std::string::npos) {
        std::string line = pendingLogLine.substr(0, end);
        pendingLogLine.erase(0, end + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const bool restarting = line.rfind("SIGUSR1[", 0) == 0 || line.rfind("SIGHUP[", 0) == 0;
        if (restarting) {
            initializationComplete = false;
            initializationFailed = false;
        } else if (line.rfind("SIGTERM[", 0) == 0 || line.rfind("SIGINT[", 0) == 0 ||
                   line.rfind("ERROR:", 0) == 0 || line.rfind("Options error:", 0) == 0 ||
                   line.rfind("Exiting due to fatal error", 0) == 0 ||
                   line.find("AUTH_FAILED") != std::string::npos ||
                   line.rfind("Initialization Sequence Completed With Errors", 0) == 0) {
            initializationComplete = false;
            initializationFailed = true;
        } else if (line == "Initialization Sequence Completed") {
            initializationComplete = !initializationFailed;
        }
    }
    if (pendingLogLine.size() > sizeof(chunk)) {
        pendingLogLine.clear();
        initializationComplete = false;
        initializationFailed = true;
    }
    // Never publish old success while newer unread events may revoke it.
    return logReadOffset == size && pendingLogLine.empty() && initializationComplete;
}

bool VPNManager::readOwnedAdapterRow(MIB_IF_ROW2& row) {
    if (!hasOwnedAdapter) return false;
    row = {};
    row.InterfaceLuid = ownedAdapterLuid;
    return GetIfEntry2(&row) == NO_ERROR &&
        IsEqualGUID(row.InterfaceGuid, ownedAdapterGuid) && row.OperStatus == IfOperStatusUp;
}

bool VPNManager::captureOwnedAdapter(const std::string& alias) {
    const std::wstring wideAlias(alias.begin(), alias.end());
    NET_LUID luid{};
    GUID guid{};
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (ConvertInterfaceAliasToLuid(wideAlias.c_str(), &luid) == NO_ERROR &&
            ConvertInterfaceLuidToGuid(&luid, &guid) == NO_ERROR) {
            std::lock_guard<std::mutex> lock(connectionMutex);
            ownedAdapterAlias = alias;
            ownedAdapterGuid = guid;
            ownedAdapterLuid = luid;
            ownedAdapterNode = guidString(guid);
            hasOwnedAdapter = !ownedAdapterNode.empty();
            return hasOwnedAdapter;
        }
        Sleep(50);
    }
    return false;
}

bool VPNManager::runAdapterTool(const std::string& path, const std::string& arguments) {
    STARTUPINFOA startup{};
    PROCESS_INFORMATION process{};
    startup.cb = sizeof(startup);
    std::string command = "\"" + path + "\" " + arguments;
    if (!CreateProcessA(path.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return false;
    const DWORD waited = WaitForSingleObject(process.hProcess, 10000);
    if (waited != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
    }
    DWORD code = 1;
    const bool success = waited == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &code) && code == 0;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return success;
}

bool VPNManager::createOwnedAdapter(const std::string& hardwareId) {
    const auto tool = findBundledExecutable("tapctl.exe");
    const auto alias = newAdapterAlias();
    if (tool.empty() || alias.empty()) return false;
    if (!runAdapterTool(tool, "create --hwid " + hardwareId + " --name \"" + alias + "\"")) return false;
    if (!captureOwnedAdapter(alias)) {
        // The unique alias belongs to this successful create operation only.
        runAdapterTool(tool, "delete \"" + alias + "\"");
        return false;
    }
    ownedAdapterToolPath = tool;
    return true;
}

void VPNManager::releaseOwnedAdapter() {
    std::string tool, node;
    {
        std::lock_guard<std::mutex> lock(connectionMutex);
        tool = ownedAdapterToolPath;
        node = ownedAdapterNode;
        hasOwnedAdapter = false;
        ownedAdapterGuid = {};
        ownedAdapterLuid = {};
        ownedAdapterNode.clear();
        ownedAdapterAlias.clear();
        ownedAdapterToolPath.clear();
        initializationComplete = false;
        initializationFailed = false;
        pendingLogLine.clear();
        logReadOffset = 0;
    }
    if (!tool.empty() && !node.empty()) runAdapterTool(tool, "delete \"" + node + "\"");
    if (wintunManager) wintunManager->destroyAdapter();
}

void VPNManager::updateStatus(const std::string& status) {
    // Event delivery stays on the platform thread; cached state is shared.
    {
        std::lock_guard<std::mutex> lock(statusMutex);
        currentStatus = status;
    }
    if (eventSink) {
        eventSink->Success(flutter::EncodableValue(status));
    }
}

void VPNManager::updateStatusThreadSafe(const std::string& status) {
    // Thread-safe method for background threads to queue status updates
    std::lock_guard<std::mutex> lock(statusMutex);
    currentStatus = status;
    pendingStatusUpdates.push(status);
    std::cout << "Queued status update: " << status << " (from background thread)" << std::endl;
}

void VPNManager::processPendingStatusUpdates() {
    // Process all pending status updates from the main thread
    std::lock_guard<std::mutex> lock(statusMutex);
    while (!pendingStatusUpdates.empty()) {
        std::string status = pendingStatusUpdates.front();
        pendingStatusUpdates.pop();
        
        // A queued success may have been revoked by a restart or child exit.
        if (status == "connected" && !checkConnectionStatus()) continue;
        currentStatus = status;
        if (eventSink) {
            eventSink->Success(flutter::EncodableValue(status));
            std::cout << "Processed status update from main thread: " << status << std::endl;
        }
    }
}

bool VPNManager::createConfigFile(const std::string& config, const std::string& username, const std::string& password) {
    try {
        char temporaryDirectory[MAX_PATH]{};
        const DWORD temporaryLength = GetTempPathA(MAX_PATH, temporaryDirectory);
        if (temporaryLength == 0 || temporaryLength >= MAX_PATH || ownedAdapterAlias.empty()) return false;
        currentConfigPath = std::string(temporaryDirectory) + ownedAdapterAlias + ".ovpn";
        currentLogPath = currentConfigPath + ".log";
        std::ofstream configFile(currentConfigPath, std::ios::trunc);
        std::ofstream freshLog(currentLogPath, std::ios::trunc);
        if (!configFile || !freshLog) return false;

        deviceType = "tun";
        std::string explicitType;
        std::istringstream input(config);
        std::string line;
        bool inlineBlock = false;
        while (std::getline(input, line)) {
            std::istringstream fields(line);
            std::string option, value;
            fields >> option >> std::quoted(value);
            if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') value = value.substr(1, value.size() - 2);
            if (option.rfind("</", 0) == 0) inlineBlock = false;
            if (inlineBlock) {
                configFile << line << '\n';
                continue;
            }
            if (!option.empty() && option.front() == '<' && option.rfind("</", 0) != 0 && option != "<connection>") inlineBlock = true;
            if (option.rfind("--", 0) == 0) option.erase(0, 2);
            if (option == "dev") deviceType = value.rfind("tap", 0) == 0 ? "tap" : "tun";
            if (option == "dev-type") explicitType = value;
            if (option == "dev" || option == "dev-type" || option == "dev-node" || option == "windows-driver" ||
                option == "log" || option == "log-append" || option == "machine-readable-output" ||
                option == "client-cert-not-required" ||
                (option == "persist-tun" && currentDriver == DriverType::WINTUN)) continue;
            configFile << line << '\n';
        }
        if (!explicitType.empty()) deviceType = explicitType;
        if ((deviceType != "tun" && deviceType != "tap") ||
            (currentDriver == DriverType::WINTUN && deviceType != "tun")) return false;
        if (!username.empty() && !password.empty()) {
            const auto authPath = currentConfigPath.substr(0, currentConfigPath.size() - 5) + "_auth.txt";
            std::ofstream auth(authPath, std::ios::trunc);
            if (!auth) return false;
            auth << username << '\n' << password;
            auto configurationAuthPath = authPath;
            std::replace(configurationAuthPath.begin(), configurationAuthPath.end(), '\\', '/');
            configFile << "auth-user-pass \"" << configurationAuthPath << "\"\n";
        }
        configFile.close();
        {
            std::lock_guard<std::mutex> lock(connectionMutex);
            logReadOffset = 0;
            pendingLogLine.clear();
            initializationComplete = false;
            initializationFailed = false;
        }
        return !configFile.fail();
    } catch (...) {
        return false;
    }
}

void VPNManager::cleanupTempFiles() {
    if (!currentLogPath.empty()) {
        DeleteFileA(currentLogPath.c_str());
        currentLogPath.clear();
    }
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

bool VPNManager::createTapAdapter() {
    // tapctl creates a dedicated TAP-Windows6 instance with a unique alias.
    // A foreign installed TAP instance is never adopted as a fallback.
    return createOwnedAdapter("root\\tap0901");
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
    if (!checkConnectionStatus()) return {0, 0};
    std::lock_guard<std::mutex> lock(connectionMutex);
    MIB_IF_ROW2 row{};
    if (!readOwnedAdapterRow(row)) return {0, 0};
    return {row.InOctets, row.OutOctets};
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
        uint64_t byteInDiff = bytesIn >= lastBytesIn ? bytesIn - lastBytesIn : 0;
        uint64_t byteOutDiff = bytesOut >= lastBytesOut ? bytesOut - lastBytesOut : 0;
        
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