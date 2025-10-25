import Cocoa
import FlutterMacOS
import NetworkExtension

public class SwiftOpenVPNFlutterPlugin: NSObject, FlutterPlugin {
    private let utils = VPNUtils()
    
    private static var EVENT_CHANNEL_VPN_STAGE = "id.laskarmedia.openvpn_flutter/vpnstage"
    private static var METHOD_CHANNEL_VPN_CONTROL = "id.laskarmedia.openvpn_flutter/vpncontrol"
     
    public static var stage: FlutterEventSink?
    private var initialized : Bool = false
    private let statsNotificationName = "com.baseserv.gcmvpn.statsUpdated" as CFString
    
    public static func register(with registrar: FlutterPluginRegistrar) {
        let instance = SwiftOpenVPNFlutterPlugin()
        instance.onRegister(registrar)
    }
    
    public func onRegister(_ registrar: FlutterPluginRegistrar){
        let vpnControlM = FlutterMethodChannel(name: SwiftOpenVPNFlutterPlugin.METHOD_CHANNEL_VPN_CONTROL, binaryMessenger: registrar.messenger)
        let vpnStageE = FlutterEventChannel(name: SwiftOpenVPNFlutterPlugin.EVENT_CHANNEL_VPN_STAGE, binaryMessenger: registrar.messenger)
        
        vpnStageE.setStreamHandler(StageHandler())
        
        // Set up Darwin notification observer for stats updates
        startDarwinStatsObserver()
        
        // Set up the stage for VPNUtils
        self.utils.stage = SwiftOpenVPNFlutterPlugin.stage
        
        vpnControlM.setMethodCallHandler({(call: FlutterMethodCall, result: @escaping FlutterResult) -> Void in
            switch call.method {
            case "status":
                guard self.utils.groupIdentifier != nil else {
                    result(nil) // Return nil if not initialized
                    return
                }
                self.utils.getTrafficStats()
                let s = UserDefaults(suiteName: self.utils.groupIdentifier!)?
                         .string(forKey: "connectionUpdate")
                result(s)
                break;
            case "stage":
                let status = self.utils.currentStatus()
                result(status)
                break;
            case "checkStatus":
                let status = self.utils.currentStatus()
                result(status)
                break;
            case "forceStatusCheck":
                // Force a status check and update
                if let providerManager = self.utils.providerManager {
                    let status = providerManager.connection.status
                    self.utils.onVpnStatusChanged(notification: status)
                }
                result("status_check_triggered")
                break;
            case "initialize":
                let providerBundleIdentifier: String? = (call.arguments as? [String: Any])?["providerBundleIdentifier"] as? String
                let localizedDescription: String? = (call.arguments as? [String: Any])?["localizedDescription"] as? String
                let groupIdentifier: String? = (call.arguments as? [String: Any])?["groupIdentifier"] as? String
                
                if providerBundleIdentifier == nil  {
                    result(FlutterError(code: "-2",
                                        message: "providerBundleIdentifier content empty or null",
                                        details: nil));
                    return;
                }
                if localizedDescription == nil  {
                    result(FlutterError(code: "-3",
                                        message: "localizedDescription content empty or null",
                                        details: nil));
                    return;
                }
                if groupIdentifier == nil  {
                    result(FlutterError(code: "-4",
                                        message: "groupIdentifier content empty or null",
                                        details: nil));
                    return;
                }
                
                self.utils.groupIdentifier = groupIdentifier
                self.utils.localizedDescription = localizedDescription
                self.utils.providerBundleIdentifier = providerBundleIdentifier
                
                self.utils.loadProviderManager{(err:Error?) in
                    if err == nil{
                        // Return current status immediately after initialization
                        let currentStatus = self.utils.currentStatus()
                        result(currentStatus)
                    }else{
                        result(FlutterError(code: "-4", message: err?.localizedDescription, details: err?.localizedDescription));
                    }
                }
                self.initialized = true
                break;
            case "disconnect":
                self.utils.stopVPN()
                result(nil)
                break;
            case "connect":
                if !self.initialized {
                    result(FlutterError(code: "-1",
                                        message: "VPNEngine need to be initialize",
                                        details: nil));
                    return
                }
                
                let config: String? = (call.arguments as? [String : Any])? ["config"] as? String
                let username: String? = (call.arguments as? [String : Any])? ["username"] as? String
                let password: String? = (call.arguments as? [String : Any])? ["password"] as? String
                
                if config == nil{
                    result(FlutterError(code: "-2",
                                        message:"Config is empty or nulled",
                                        details: "Config can't be nulled"))
                    return
                }
                
                self.utils.configureVPN(config: config, username: username, password: password, completion: {(success:Error?) -> Void in
                    if(success == nil){
                        result(nil)
                    }else{
                        result(FlutterError(code: "99",
                                            message: "permission denied",
                                            details: success?.localizedDescription))
                    }
                })
                break;
            case "dispose":
                // Clean up VPN status observer
                if let observer = self.utils.vpnStageObserver {
                    NotificationCenter.default.removeObserver(observer)
                    self.utils.vpnStageObserver = nil
                }
                self.initialized = false
                result(nil)
                break;
            default:
                result(FlutterMethodNotImplemented)
                break;
            }
        })
    }
    
    private func startDarwinStatsObserver() {
        CFNotificationCenterAddObserver(
            CFNotificationCenterGetDarwinNotifyCenter(),
            Unmanaged.passUnretained(self).toOpaque(),
            { _, observer, name, _, _ in
                let this = Unmanaged<SwiftOpenVPNFlutterPlugin>
                    .fromOpaque(observer!).takeUnretainedValue()
                this.utils.getTrafficStats()
            },
            statsNotificationName,
            nil,
            .deliverImmediately
        )
        print("📡 OpenVPN Plugin: Set up Darwin notification listener for VPN stats")
    }
    
    deinit {
        // Clean up Darwin notification observer
        CFNotificationCenterRemoveObserver(
            CFNotificationCenterGetDarwinNotifyCenter(),
            Unmanaged.passUnretained(self).toOpaque(),
            statsNotificationName,
            nil
        )
    }
    
    
    class StageHandler: NSObject, FlutterStreamHandler {
        func onListen(withArguments arguments: Any?, eventSink events: @escaping FlutterEventSink) -> FlutterError? {
            SwiftOpenVPNFlutterPlugin.stage = events
            
            // Immediately send current status if available
            events("disconnected") // Default status
            
            return nil
        }
        
        func onCancel(withArguments arguments: Any?) -> FlutterError? {
            SwiftOpenVPNFlutterPlugin.stage = nil
            return nil
        }
    }
    
    
}


@available(macOS 10.14, *)
class VPNUtils {
    var providerManager: NETunnelProviderManager!
    var providerBundleIdentifier : String?
    var localizedDescription : String?
    var groupIdentifier : String?
    var stage : FlutterEventSink!
    var vpnStageObserver : NSObjectProtocol?
    
    func loadProviderManager(completion:@escaping (_ error : Error?) -> Void)  {
        NETunnelProviderManager.loadAllFromPreferences { (managers, error)  in
            if error == nil {
                self.providerManager = managers?.first ?? NETunnelProviderManager()
                
                // Set up VPN status observer (don't filter by object to avoid missing updates)
                self.vpnStageObserver = NotificationCenter.default.addObserver(
                    forName: .NEVPNStatusDidChange,
                    object: nil,  // Receive regardless of connection instance
                    queue: .main
                ) { [weak self] _ in
                    let status = self?.providerManager.connection.status ?? .invalid
                    self?.onVpnStatusChanged(notification: status)
                }
                
                // Check current status immediately
                let currentStatus = self.providerManager.connection.status
                if currentStatus != .invalid {
                    self.onVpnStatusChanged(notification: currentStatus)
                }
                
                completion(nil)
            } else {
                completion(error)
            }
        }
    }
    
    func onVpnStatusChanged(notification : NEVPNStatus) {
        // Try both stage callbacks (static and instance)
        let stageCallbacks = [stage, SwiftOpenVPNFlutterPlugin.stage].compactMap { $0 }
        
        if stageCallbacks.isEmpty {
            return
        }
        
        let statusString = onVpnStatusChangedString(notification: notification)
        
        // Send status update only once per event
        for callback in stageCallbacks {
            callback(statusString ?? "disconnected")
        }
    }
    
    func onVpnStatusChangedString(notification : NEVPNStatus?) -> String?{
        if notification == nil {
            return "disconnected"
        }
        switch notification! {
        case NEVPNStatus.connected:
            return "connected";
        case NEVPNStatus.connecting:
            return "connecting";
        case NEVPNStatus.disconnected:
            return "disconnected";
        case NEVPNStatus.disconnecting:
            return "disconnecting";
        case NEVPNStatus.invalid:
            return "invalid";
        case NEVPNStatus.reasserting:
            return "reasserting";
        default:
            return "disconnected";
        }
    }
    
    func currentStatus() -> String? {
        if self.providerManager != nil {
            let status = self.providerManager.connection.status
            let statusString = onVpnStatusChangedString(notification: status)
            return statusString
        }
        return "disconnected"
    }
    
    func configureVPN(config: String?, username : String?,password : String?,completion:@escaping (_ error : Error?) -> Void) {
        let configData = config
        
        self.providerManager?.loadFromPreferences { error in
            if error == nil {
                let tunnelProtocol = NETunnelProviderProtocol()
                tunnelProtocol.serverAddress = "" // Use empty string like iOS
                tunnelProtocol.providerBundleIdentifier = self.providerBundleIdentifier
                
                // Use String keys to match the extension expectations
                tunnelProtocol.providerConfiguration = [
                    "vpn_config": configData ?? "",
                    "groupIdentifier": self.groupIdentifier ?? "",
                    "username": username ?? "",
                    "password": password ?? ""
                ]
                tunnelProtocol.disconnectOnSleep = false
                
                self.providerManager.protocolConfiguration = tunnelProtocol
                self.providerManager.localizedDescription = self.localizedDescription
                self.providerManager.isEnabled = true
                self.providerManager.saveToPreferences(completionHandler: { (error) in
                    if error == nil {
                        self.providerManager.loadFromPreferences(completionHandler: { (error) in
                            if let error = error {
                                completion(error)
                                return
                            }
                            
                            do {
                                // Use the same approach as iOS for starting the tunnel
                                if username != nil && password != nil {
                                    let options: [String : NSObject] = [
                                        "username": username! as NSString,
                                        "password": password! as NSString
                                    ]
                                    try self.providerManager.connection.startVPNTunnel(options: options)
                                } else {
                                    try self.providerManager.connection.startVPNTunnel()
                                }
                                completion(nil)
                            } catch {
                                completion(error)
                            }
                        })
                    } else {
                        completion(error)
                    }
                })
            } else {
                completion(error)
            }
        }
    }
    
    func stopVPN() {
        // Stop the tunnel immediately
        self.providerManager.connection.stopVPNTunnel()
        
        // Clear connection update to avoid showing stale stats
        if let group = groupIdentifier,
           let sharedDefaults = UserDefaults(suiteName: group) {
            sharedDefaults.removeObject(forKey: "connectionUpdate")
            sharedDefaults.synchronize()
        }
    }
    
    func getTrafficStats() {
        guard let group = groupIdentifier,
              let sharedDefaults = UserDefaults(suiteName: group) else { return }
        
        // Pull freshest values written by the packet tunnel
        sharedDefaults.synchronize()
        
        // Try to get comprehensive statistics first
        if let vpnStats = sharedDefaults.dictionary(forKey: "vpn_statistics") {
                // Use the comprehensive statistics with speed data
                let formatter = DateFormatter()
                formatter.dateFormat = "yyyy-MM-dd HH:mm:ss"
                
                let connectedDate = vpnStats["connected_on"] as? String ?? formatter.string(from: Date())
                
                // Handle different data types for byte counts
                let bytes_in: String
                let bytes_out: String
                let packets_in: String
                let packets_out: String
                
                if let bytesInUInt64 = vpnStats["byte_in"] as? UInt64 {
                    bytes_in = String(bytesInUInt64)
                } else if let bytesInString = vpnStats["byte_in"] as? String {
                    bytes_in = bytesInString
                } else {
                    bytes_in = "0"
                }
                
                if let bytesOutUInt64 = vpnStats["byte_out"] as? UInt64 {
                    bytes_out = String(bytesOutUInt64)
                } else if let bytesOutString = vpnStats["byte_out"] as? String {
                    bytes_out = bytesOutString
                } else {
                    bytes_out = "0"
                }
                
                if let packetsInUInt64 = vpnStats["packets_in"] as? UInt64 {
                    packets_in = String(packetsInUInt64)
                } else if let packetsInString = vpnStats["packets_in"] as? String {
                    packets_in = packetsInString
                } else {
                    packets_in = "0"
                }
                
                if let packetsOutUInt64 = vpnStats["packets_out"] as? UInt64 {
                    packets_out = String(packetsOutUInt64)
                } else if let packetsOutString = vpnStats["packets_out"] as? String {
                    packets_out = packetsOutString
                } else {
                    packets_out = "0"
                }
                
                // Handle speed data (should be Double from VPN extension)
                let speed_in = vpnStats["speed_in_mbps"] as? Double ?? 0.0
                let speed_out = vpnStats["speed_out_mbps"] as? Double ?? 0.0
                
                // Create enhanced connection update with speed data
                let connectionUpdate = "\(connectedDate)_\(packets_in)_\(packets_out)_\(bytes_in)_\(bytes_out)_\(String(format: "%.2f", speed_in))_\(String(format: "%.2f", speed_out))"
                sharedDefaults.set(connectionUpdate, forKey: "connectionUpdate")
                
                // Push back to disk so Flutter can read immediately
                sharedDefaults.synchronize()
                
                // Debug logging
                print("🔧 Flutter Plugin: Reading VPN stats:")
                print("   Connected: \(connectedDate)")
                print("   Bytes: In=\(bytes_in), Out=\(bytes_out)")
                print("   Packets: In=\(packets_in), Out=\(packets_out)")
                print("   Speeds: DL=\(String(format: "%.2f", speed_in)) Mbps, UL=\(String(format: "%.2f", speed_out)) Mbps")
                print("   ConnectionUpdate: \(connectionUpdate)")
            } else {
                // Fallback to original method
                let formatter = DateFormatter()
                formatter.dateFormat = "yyyy-MM-dd HH:mm:ss"
                
                let connectedDate = sharedDefaults.object(forKey: "connected_date") as? Date ?? Date()
                let bytes_in = sharedDefaults.object(forKey: "bytes_in") as? String ?? "0"
                let bytes_out = sharedDefaults.object(forKey: "bytes_out") as? String ?? "0"
                let packets_in = sharedDefaults.object(forKey: "packets_in") as? String ?? "0"
                let packets_out = sharedDefaults.object(forKey: "packets_out") as? String ?? "0"
                
                let connectionUpdate = "\(formatter.string(from: connectedDate))_\(packets_in)_\(packets_out)_\(bytes_in)_\(bytes_out)_0.0_0.0"
                sharedDefaults.set(connectionUpdate, forKey: "connectionUpdate")
                
                // Push back to disk so Flutter can read immediately
                sharedDefaults.synchronize()
                
                print("🔧 Flutter Plugin: Using fallback stats (no vpn_statistics found)")
            }
        }
    }
} 