#pragma once

#include <windows.h>
#include <string>
#include <memory>

// WinTun API definitions - based on official WinTun.h
typedef struct _WINTUN_ADAPTER* WINTUN_ADAPTER_HANDLE;
typedef struct _WINTUN_SESSION* WINTUN_SESSION_HANDLE;

typedef struct
{
    DWORD Size;
} WINTUN_PACKET;

// WinTun function pointers
typedef WINTUN_ADAPTER_HANDLE(WINAPI *WINTUN_CREATE_ADAPTER_FUNC)(LPCWSTR Name, LPCWSTR TunnelType, const GUID *RequestedGUID);
typedef BOOL(WINAPI *WINTUN_CLOSE_ADAPTER_FUNC)(WINTUN_ADAPTER_HANDLE Adapter);
typedef WINTUN_SESSION_HANDLE(WINAPI *WINTUN_START_SESSION_FUNC)(WINTUN_ADAPTER_HANDLE Adapter, DWORD Capacity);
typedef void(WINAPI *WINTUN_END_SESSION_FUNC)(WINTUN_SESSION_HANDLE Session);
typedef BYTE*(WINAPI *WINTUN_ALLOCATE_SEND_PACKET_FUNC)(WINTUN_SESSION_HANDLE Session, DWORD PacketSize);
typedef void(WINAPI *WINTUN_SEND_PACKET_FUNC)(WINTUN_SESSION_HANDLE Session, const BYTE *Packet);
typedef BYTE*(WINAPI *WINTUN_RECEIVE_PACKET_FUNC)(WINTUN_SESSION_HANDLE Session, DWORD *PacketSize);
typedef void(WINAPI *WINTUN_RELEASE_RECEIVE_PACKET_FUNC)(WINTUN_SESSION_HANDLE Session, const BYTE *Packet);
typedef HANDLE(WINAPI *WINTUN_GET_READ_WAIT_EVENT_FUNC)(WINTUN_SESSION_HANDLE Session);
typedef DWORD(WINAPI *WINTUN_GET_RUNNING_DRIVER_VERSION_FUNC)(void);

namespace openvpn_flutter {

class WinTunManager {
private:
    HMODULE wintunDll = NULL;
    WINTUN_ADAPTER_HANDLE adapter = NULL;
    WINTUN_SESSION_HANDLE session = NULL;
    std::string adapterName;
    GUID adapterGuid;
    
    // WinTun function pointers
    WINTUN_CREATE_ADAPTER_FUNC WinTunCreateAdapter = nullptr;
    WINTUN_CLOSE_ADAPTER_FUNC WinTunCloseAdapter = nullptr;
    WINTUN_START_SESSION_FUNC WinTunStartSession = nullptr;
    WINTUN_END_SESSION_FUNC WinTunEndSession = nullptr;
    WINTUN_GET_RUNNING_DRIVER_VERSION_FUNC WinTunGetRunningDriverVersion = nullptr;
    
public:
    WinTunManager();
    ~WinTunManager();
    
    bool initialize();
    bool isWinTunAvailable();
    bool createAdapter(const std::string& name);
    bool destroyAdapter();
    bool startSession();
    void endSession();
    std::string getAdapterName() const;
    DWORD getDriverVersion();
    
private:
    bool loadWinTunDll();
    void unloadWinTunDll();
    bool loadWinTunFunctions();
    std::string generateAdapterName();
    GUID generateGuid();
};

} // namespace openvpn_flutter 