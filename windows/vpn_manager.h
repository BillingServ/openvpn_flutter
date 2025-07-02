#pragma once

#include <windows.h>
#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <flutter/event_channel.h>
#include "wintun_manager.h"

namespace openvpn_flutter {

enum class DriverType {
    WINTUN,
    TAP_WINDOWS
};

class VPNManager {
private:
    PROCESS_INFORMATION processInfo;
    HANDLE hProcess = NULL;
    std::atomic<bool> isConnected{false};
    std::atomic<bool> isConnecting{false};
    std::string currentConfigPath;
    std::string currentStatus = "disconnected";
    std::thread statusMonitorThread;
    std::atomic<bool> shouldMonitor{false};
    flutter::EventSink<flutter::EncodableValue>* eventSink = nullptr;
    
    // Driver management
    DriverType preferredDriver = DriverType::WINTUN;
    bool allowFallbackToTAP = true;
    DriverType currentDriver = DriverType::WINTUN;
    
    // WinTun management
    std::unique_ptr<WinTunManager> wintunManager;
    
    // TAP adapter management (fallback)
    std::string tapAdapterName;
    bool tapDriverInstalled = false;
    
public:
    VPNManager();
    ~VPNManager();
    
    void setEventSink(flutter::EventSink<flutter::EncodableValue>* sink);
    bool startVPN(const std::string& config, const std::string& username = "", const std::string& password = "");
    void stopVPN();
    std::string getStatus();
    std::string getConnectionStats();
    
    // Driver management
    bool initializeDriver();
    bool initializeWinTun();
    bool initializeTapDriver();
    bool installTapDriver();
    bool isWinTunAvailable();
    bool isTapDriverInstalled();
    DriverType getCurrentDriver() const;
    void setPreferredDriver(DriverType type, bool allowFallback = true);
    
private:
    std::string getBundledOpenVPNPath();
    std::string findBundledExecutable(const std::string& filename);
    void monitorConnection();
    void updateStatus(const std::string& status);
    bool createConfigFile(const std::string& config, const std::string& username, const std::string& password);
    void cleanupTempFiles();
    
    // TAP adapter utilities
    std::string findTapAdapter();
    bool createTapAdapter();
    bool enableTapAdapter();
    bool disableTapAdapter();
    
    // Registry and system utilities
    bool runAsAdmin(const std::string& command, const std::string& params = "");
    bool isRunningAsAdmin();
    std::string getAppDirectory();
};

} // namespace openvpn_flutter 