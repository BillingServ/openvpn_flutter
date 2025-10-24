import Cocoa
import FlutterMacOS
import NetworkExtension

public class SwiftOpenVPNFlutterPlugin: NSObject, FlutterPlugin {
    private static var utils : VPNUtils! = VPNUtils()
    
    private static var EVENT_CHANNEL_VPN_STAGE = "id.laskarmedia.openvpn_flutter/vpnstage"
    private static var METHOD_CHANNEL_VPN_CONTROL = "id.laskarmedia.openvpn_flutter/vpncontrol"
     
    public static var stage: FlutterEventSink?
    private var initialized : Bool = false
    
    public static func register(with registrar: FlutterPluginRegistrar) {
        let instance = SwiftOpenVPNFlutterPlugin()
        instance.onRegister(registrar)
    }
    
    public func onRegister(_ registrar: FlutterPluginRegistrar){
        let vpnControlM = FlutterMethodChannel(name: SwiftOpenVPNFlutterPlugin.METHOD_CHANNEL_VPN_CONTROL, binaryMessenger: registrar.messenger)
        let vpnStageE = FlutterEventChannel(name: SwiftOpenVPNFlutterPlugin.EVENT_CHANNEL_VPN_STAGE, binaryMessenger: registrar.messenger)
        
        vpnStageE.setStreamHandler(StageHandler())
        vpnControlM.setMethodCallHandler({(call: FlutterMethodCall, result: @escaping FlutterResult) -> Void in
            switch call.method {
            case "status":
                SwiftOpenVPNFlutterPlugin.utils.getTraffictStats()
                result(UserDefaults.init(suiteName: SwiftOpenVPNFlutterPlugin.utils.groupIdentifier)?.string(forKey: "connectionUpdate"))
                break;
            case "stage":
                let status = SwiftOpenVPNFlutterPlugin.utils.currentStatus()
                result(status)
                break;
            case "checkStatus":
                let status = SwiftOpenVPNFlutterPlugin.utils.currentStatus()
                result(status)
                break;
            case "forceStatusCheck":
                // Force a status check and update
                if let providerManager = SwiftOpenVPNFlutterPlugin.utils.providerManager {
                    let status = providerManager.connection.status
                    SwiftOpenVPNFlutterPlugin.utils.onVpnStatusChanged(notification: status)
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
                
                SwiftOpenVPNFlutterPlugin.utils.groupIdentifier = groupIdentifier
                SwiftOpenVPNFlutterPlugin.utils.localizedDescription = localizedDescription
                SwiftOpenVPNFlutterPlugin.utils.providerBundleIdentifier = providerBundleIdentifier
                
                SwiftOpenVPNFlutterPlugin.utils.loadProviderManager{(err:Error?) in
                    if err == nil{
                        // Return current status immediately after initialization
                        let currentStatus = SwiftOpenVPNFlutterPlugin.utils.currentStatus()
                        result(currentStatus)
                    }else{
                        result(FlutterError(code: "-4", message: err?.localizedDescription, details: err?.localizedDescription));
                    }
                }
                self.initialized = true
                break;
            case "disconnect":
                SwiftOpenVPNFlutterPlugin.utils.stopVPN()
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
                
                SwiftOpenVPNFlutterPlugin.utils.configureVPN(config: config, username: username, password: password, completion: {(success:Error?) -> Void in
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
                if let observer = SwiftOpenVPNFlutterPlugin.utils.vpnStageObserver {
                    NotificationCenter.default.removeObserver(observer)
                    SwiftOpenVPNFlutterPlugin.utils.vpnStageObserver = nil
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
    
    
    class StageHandler: NSObject, FlutterStreamHandler {
        func onListen(withArguments arguments: Any?, eventSink events: @escaping FlutterEventSink) -> FlutterError? {
            SwiftOpenVPNFlutterPlugin.stage = events
            SwiftOpenVPNFlutterPlugin.utils.stage = events
            
            // Immediately send current status if available
            if let currentStatus = SwiftOpenVPNFlutterPlugin.utils.currentStatus() {
                events(currentStatus)
            }
            
            return nil
        }
        
        func onCancel(withArguments arguments: Any?) -> FlutterError? {
            SwiftOpenVPNFlutterPlugin.stage = nil
            SwiftOpenVPNFlutterPlugin.utils.stage = nil
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
                
                // Set up VPN status observer
                self.vpnStageObserver = NotificationCenter.default.addObserver(
                    forName: .NEVPNStatusDidChange,
                    object: self.providerManager.connection,
                    queue: .main
                ) { [weak self] notification in
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
                
                // Use the same configuration approach as iOS
                let nullData = "".data(using: .utf8)
                tunnelProtocol.providerConfiguration = [
                    "config": configData?.data(using: .utf8) ?? nullData!,
                    "groupIdentifier": self.groupIdentifier?.data(using: .utf8) ?? nullData!,
                    "username": username?.data(using: .utf8) ?? nullData!,
                    "password": password?.data(using: .utf8) ?? nullData!
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
    }
    
    func getTraffictStats() {
        if let sharedDefaults = UserDefaults.init(suiteName: groupIdentifier) {
            // Try to get comprehensive statistics first
            if let vpnStats = sharedDefaults.dictionary(forKey: "vpn_statistics") {
                // Use the comprehensive statistics with speed data
                let formatter = DateFormatter()
                formatter.dateFormat = "yyyy-MM-dd HH:mm:ss"
                
                let connectedDate = vpnStats["connected_on"] as? String ?? formatter.string(from: Date())
                let bytes_in = vpnStats["byte_in"] as? String ?? "0"
                let bytes_out = vpnStats["byte_out"] as? String ?? "0"
                let packets_in = vpnStats["packets_in"] as? String ?? "0"
                let packets_out = vpnStats["packets_out"] as? String ?? "0"
                let speed_in = vpnStats["speed_in_mbps"] as? Double ?? 0.0
                let speed_out = vpnStats["speed_out_mbps"] as? Double ?? 0.0
                
                // Create enhanced connection update with speed data
                let connectionUpdate = "\(connectedDate)_\(packets_in)_\(packets_out)_\(bytes_in)_\(bytes_out)_\(String(format: "%.2f", speed_in))_\(String(format: "%.2f", speed_out))"
                sharedDefaults.set(connectionUpdate, forKey: "connectionUpdate")
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
            }
        }
    }
} 