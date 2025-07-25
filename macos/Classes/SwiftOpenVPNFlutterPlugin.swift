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
        print("🔧 macOS OpenVPN Plugin: Plugin registration started")
        let instance = SwiftOpenVPNFlutterPlugin()
        instance.onRegister(registrar)
        print("🔧 macOS OpenVPN Plugin: Plugin registration completed")
    }
    
    public func onRegister(_ registrar: FlutterPluginRegistrar){
        print("🔧 macOS OpenVPN Plugin: Setting up method channels...")
        let vpnControlM = FlutterMethodChannel(name: SwiftOpenVPNFlutterPlugin.METHOD_CHANNEL_VPN_CONTROL, binaryMessenger: registrar.messenger)
        let vpnStageE = FlutterEventChannel(name: SwiftOpenVPNFlutterPlugin.EVENT_CHANNEL_VPN_STAGE, binaryMessenger: registrar.messenger)
        print("🔧 macOS OpenVPN Plugin: Method channels created successfully")
        
        vpnStageE.setStreamHandler(StageHandler())
        vpnControlM.setMethodCallHandler({(call: FlutterMethodCall, result: @escaping FlutterResult) -> Void in
            print("🔧 macOS OpenVPN Plugin: Method called: \(call.method)")
            switch call.method {
            case "status":
                SwiftOpenVPNFlutterPlugin.utils.getTraffictStats()
                result(UserDefaults.init(suiteName: SwiftOpenVPNFlutterPlugin.utils.groupIdentifier)?.string(forKey: "connectionUpdate"))
                break;
            case "stage":
                print("🔧 macOS OpenVPN Plugin: Stage method called")
                let status = SwiftOpenVPNFlutterPlugin.utils.currentStatus()
                print("🔧 macOS OpenVPN Plugin: Returning stage: \(status ?? "nil")")
                result(status)
                break;
            case "checkStatus":
                print("🔧 macOS OpenVPN Plugin: Check status method called")
                let status = SwiftOpenVPNFlutterPlugin.utils.currentStatus()
                print("🔧 macOS OpenVPN Plugin: Check status result: \(status ?? "nil")")
                result(status)
                break;
            case "forceStatusCheck":
                print("🔧 macOS OpenVPN Plugin: Force status check method called")
                // Force a status check and update
                if let providerManager = SwiftOpenVPNFlutterPlugin.utils.providerManager {
                    let status = providerManager.connection.status
                    print("🔧 macOS OpenVPN Plugin: Forcing status update: \(status.rawValue)")
                    SwiftOpenVPNFlutterPlugin.utils.onVpnStatusChanged(notification: status)
                }
                result("status_check_triggered")
                break;
            case "initialize":
                print("🔧 macOS OpenVPN Plugin: Initialize method called")
                let providerBundleIdentifier: String? = (call.arguments as? [String: Any])?["providerBundleIdentifier"] as? String
                let localizedDescription: String? = (call.arguments as? [String: Any])?["localizedDescription"] as? String
                let groupIdentifier: String? = (call.arguments as? [String: Any])?["groupIdentifier"] as? String
                
                print("🔧 macOS OpenVPN Plugin: Group identifier: \(groupIdentifier ?? "nil")")
                print("🔧 macOS OpenVPN Plugin: Provider bundle identifier: \(providerBundleIdentifier ?? "nil")")
                print("🔧 macOS OpenVPN Plugin: Localized description: \(localizedDescription ?? "nil")")
                if providerBundleIdentifier == nil  {
                    print("🔧 macOS OpenVPN Plugin: providerBundleIdentifier is nil")
                    result(FlutterError(code: "-2",
                                        message: "providerBundleIdentifier content empty or null",
                                        details: nil));
                    return;
                }
                if localizedDescription == nil  {
                    print("🔧 macOS OpenVPN Plugin: localizedDescription is nil")
                    result(FlutterError(code: "-3",
                                        message: "localizedDescription content empty or null",
                                        details: nil));
                    return;
                }
                if groupIdentifier == nil  {
                    print("🔧 macOS OpenVPN Plugin: groupIdentifier is nil")
                    result(FlutterError(code: "-4",
                                        message: "groupIdentifier content empty or null",
                                        details: nil));
                    return;
                }
                print("🔧 macOS OpenVPN Plugin: Setting up VPN utils...")
                SwiftOpenVPNFlutterPlugin.utils.groupIdentifier = groupIdentifier
                SwiftOpenVPNFlutterPlugin.utils.localizedDescription = localizedDescription
                SwiftOpenVPNFlutterPlugin.utils.providerBundleIdentifier = providerBundleIdentifier
                
                print("🔧 macOS OpenVPN Plugin: Loading provider manager...")
                SwiftOpenVPNFlutterPlugin.utils.loadProviderManager{(err:Error?) in
                    if err == nil{
                        print("🔧 macOS OpenVPN Plugin: Provider manager loaded successfully")
                        
                        // Return current status immediately after initialization
                        let currentStatus = SwiftOpenVPNFlutterPlugin.utils.currentStatus()
                        print("🔧 macOS OpenVPN Plugin: Initial status: \(currentStatus ?? "nil")")
                        result(currentStatus)
                    }else{
                        print("🔧 macOS OpenVPN Plugin: Failed to load provider manager: \(err?.localizedDescription ?? "unknown error")")
                        result(FlutterError(code: "-4", message: err?.localizedDescription, details: err?.localizedDescription));
                    }
                }
                self.initialized = true
                break;
            case "disconnect":
                print("🔧 macOS OpenVPN Plugin: Disconnect method called")
                SwiftOpenVPNFlutterPlugin.utils.stopVPN()
                result(nil)
                break;
            case "connect":
                print("🔧 macOS OpenVPN Plugin: Connect method called")
                if !self.initialized {
                    print("🔧 macOS OpenVPN Plugin: VPN not initialized")
                    result(FlutterError(code: "-1",
                                        message: "VPNEngine need to be initialize",
                                        details: nil));
                    return
                }
                

                let config: String? = (call.arguments as? [String : Any])? ["config"] as? String
                let username: String? = (call.arguments as? [String : Any])? ["username"] as? String
                let password: String? = (call.arguments as? [String : Any])? ["password"] as? String
                
                print("🔧 macOS OpenVPN Plugin: Config length: \(config?.count ?? 0)")
                print("🔧 macOS OpenVPN Plugin: Username: \(username ?? "nil")")
                print("🔧 macOS OpenVPN Plugin: Password provided: \(password != nil)")
                
                if config == nil{
                    print("🔧 macOS OpenVPN Plugin: Config is nil")
                    result(FlutterError(code: "-2",
                                        message:"Config is empty or nulled",
                                        details: "Config can't be nulled"))
                    return
                }
                

                
                print("🔧 macOS OpenVPN Plugin: Calling configureVPN...")
                SwiftOpenVPNFlutterPlugin.utils.configureVPN(config: config, username: username, password: password, completion: {(success:Error?) -> Void in
                    if(success == nil){
                        print("🔧 macOS OpenVPN Plugin: configureVPN completed successfully")
                        result(nil)
                    }else{
                        print("🔧 macOS OpenVPN Plugin: configureVPN failed: \(success?.localizedDescription ?? "unknown error")")
                        result(FlutterError(code: "99",
                                            message: "permission denied",
                                            details: success?.localizedDescription))
                    }
                })
                break;
            case "dispose":
                print("🔧 macOS OpenVPN Plugin: Dispose method called")
                // Clean up VPN status observer
                if let observer = SwiftOpenVPNFlutterPlugin.utils.vpnStageObserver {
                    print("🔧 macOS OpenVPN Plugin: Removing VPN status observer")
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
            print("🔧 macOS OpenVPN Plugin: StageHandler onListen called")
            SwiftOpenVPNFlutterPlugin.stage = events
            SwiftOpenVPNFlutterPlugin.utils.stage = events
            print("🔧 macOS OpenVPN Plugin: Stage callback set successfully")
            
            // Immediately send current status if available
            if let currentStatus = SwiftOpenVPNFlutterPlugin.utils.currentStatus() {
                print("🔧 macOS OpenVPN Plugin: Sending initial status: \(currentStatus)")
                events(currentStatus)
            }
            
            return nil
        }
        
        func onCancel(withArguments arguments: Any?) -> FlutterError? {
            print("🔧 macOS OpenVPN Plugin: StageHandler onCancel called")
            SwiftOpenVPNFlutterPlugin.stage = nil
            SwiftOpenVPNFlutterPlugin.utils.stage = nil
            print("🔧 macOS OpenVPN Plugin: Stage callback cleared")
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
        print("🔧 macOS OpenVPN Plugin: Loading provider manager...")
        print("🔧 macOS OpenVPN Plugin: Provider bundle identifier: \(self.providerBundleIdentifier ?? "nil")")
        
                        // Check if the provider bundle is available
                if let bundleId = self.providerBundleIdentifier {
                    let bundle = Bundle(identifier: bundleId)
                    if bundle != nil {
                        print("🔧 macOS OpenVPN Plugin: Provider bundle found: \(bundleId)")
                    } else {
                        print("🔧 macOS OpenVPN Plugin: WARNING - Provider bundle not found: \(bundleId)")
                        print("🔧 macOS OpenVPN Plugin: This may cause connection failures")
                    }
                }
        
        NETunnelProviderManager.loadAllFromPreferences { (managers, error)  in
            if error == nil {
                print("🔧 macOS OpenVPN Plugin: Found \(managers?.count ?? 0) existing VPN configurations")
                
                // Check for existing configurations
                if let existingManagers = managers, !existingManagers.isEmpty {
                    for (index, manager) in existingManagers.enumerated() {
                        print("🔧 macOS OpenVPN Plugin: Existing config \(index): \(manager.localizedDescription ?? "unnamed")")
                        print("🔧 macOS OpenVPN Plugin:   - Enabled: \(manager.isEnabled)")
                        print("🔧 macOS OpenVPN Plugin:   - Status: \(manager.connection.status.rawValue)")
                    }
                }
                
                self.providerManager = managers?.first ?? NETunnelProviderManager()
                print("🔧 macOS OpenVPN Plugin: Provider manager loaded successfully")
                
                // Set up VPN status observer
                print("🔧 macOS OpenVPN Plugin: Setting up VPN status observer...")
                self.vpnStageObserver = NotificationCenter.default.addObserver(
                    forName: .NEVPNStatusDidChange,
                    object: self.providerManager.connection,
                    queue: .main
                ) { [weak self] notification in
                    print("🔧 macOS OpenVPN Plugin: VPN status notification received!")
                    let status = self?.providerManager.connection.status ?? .invalid
                    print("🔧 macOS OpenVPN Plugin: Current VPN status: \(status.rawValue)")
                    
                    // Additional logging for disconnect process
                    if status == .disconnected {
                        print("🔧 macOS OpenVPN Plugin: VPN fully disconnected")
                    } else if status == .disconnecting {
                        print("🔧 macOS OpenVPN Plugin: VPN is disconnecting...")
                    }
                    
                    self?.onVpnStatusChanged(notification: status)
                }
                
                print("🔧 macOS OpenVPN Plugin: VPN status observer set up successfully")
                
                // Check current status immediately
                let currentStatus = self.providerManager.connection.status
                print("🔧 macOS OpenVPN Plugin: Initial VPN status: \(currentStatus.rawValue)")
                if currentStatus != .invalid {
                    print("🔧 macOS OpenVPN Plugin: Calling onVpnStatusChanged with initial status")
                    self.onVpnStatusChanged(notification: currentStatus)
                }
                
                completion(nil)
            } else {
                print("🔧 macOS OpenVPN Plugin: Error loading provider manager: \(error?.localizedDescription ?? "unknown error")")
                completion(error)
            }
        }
    }
    
    func onVpnStatusChanged(notification : NEVPNStatus) {
        print("🔧 macOS OpenVPN Plugin: VPN status changed to: \(notification.rawValue)")
        print("🔧 macOS OpenVPN Plugin: About to call stage callback with status")
        print("🔧 macOS OpenVPN Plugin: Stage callback is nil: \(stage == nil)")
        
        // Try both stage callbacks (static and instance)
        let stageCallbacks = [stage, SwiftOpenVPNFlutterPlugin.stage].compactMap { $0 }
        
        if stageCallbacks.isEmpty {
            print("🔧 macOS OpenVPN Plugin: ERROR - No stage callbacks available!")
            return
        }
        
        let statusString = onVpnStatusChangedString(notification: notification)
        print("🔧 macOS OpenVPN Plugin: Status string: \(statusString ?? "nil")")
        
        for callback in stageCallbacks {
            print("🔧 macOS OpenVPN Plugin: Calling stage callback with: \(statusString ?? "nil")")
            callback(statusString ?? "disconnected")
        }
        
        print("🔧 macOS OpenVPN Plugin: All stage callbacks called successfully")
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
            print("🔧 macOS OpenVPN Plugin: Current status: \(statusString ?? "nil") (raw: \(status.rawValue))")
            
            // Add more detailed status information
            switch status {
            case .invalid:
                print("🔧 macOS OpenVPN Plugin: Status is invalid - VPN may not be properly configured")
            case .disconnected:
                print("🔧 macOS OpenVPN Plugin: VPN is disconnected")
            case .connecting:
                print("🔧 macOS OpenVPN Plugin: VPN is connecting...")
            case .connected:
                print("🔧 macOS OpenVPN Plugin: VPN is connected")
            case .disconnecting:
                print("🔧 macOS OpenVPN Plugin: VPN is disconnecting...")
            case .reasserting:
                print("🔧 macOS OpenVPN Plugin: VPN is reasserting...")
            @unknown default:
                print("🔧 macOS OpenVPN Plugin: Unknown VPN status: \(status.rawValue)")
            }
            
            return statusString
        }
        return "disconnected"
    }
    
    func configureVPN(config: String?, username : String?,password : String?,completion:@escaping (_ error : Error?) -> Void) {
        let configData = config
        print("🔧 macOS OpenVPN Plugin: Configuring VPN with config length: \(configData?.count ?? 0)")
        print("🔧 macOS OpenVPN Plugin: Username: \(username ?? "nil")")
        print("🔧 macOS OpenVPN Plugin: Password: \(password != nil ? "provided" : "nil")")
        
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
                
                if let config = tunnelProtocol.providerConfiguration {
                    print("🔧 macOS OpenVPN Plugin: Provider configuration set with keys: \(Array(config.keys))")
                    print("🔧 macOS OpenVPN Plugin: Config data length: \(configData?.count ?? 0)")
                } else {
                    print("🔧 macOS OpenVPN Plugin: Provider configuration is nil")
                }
                self.providerManager.protocolConfiguration = tunnelProtocol
                self.providerManager.localizedDescription = self.localizedDescription
                self.providerManager.isEnabled = true
                self.providerManager.saveToPreferences(completionHandler: { (error) in
                    if error == nil {
                        print("🔧 macOS OpenVPN Plugin: Preferences saved successfully")
                        self.providerManager.loadFromPreferences(completionHandler: { (error) in
                            if let error = error {
                                print("🔧 macOS OpenVPN Plugin: Failed to load preferences after save: \(error.localizedDescription)")
                                completion(error)
                                return
                            } else {
                                print("🔧 macOS OpenVPN Plugin: Preferences loaded successfully after save")
                            }
                            
                            do {
                                print("🔧 macOS OpenVPN Plugin: Starting VPN tunnel...")
                                print("🔧 macOS OpenVPN Plugin: Provider bundle ID: \(self.providerBundleIdentifier ?? "nil")")
                                print("🔧 macOS OpenVPN Plugin: Server address: \(tunnelProtocol.serverAddress ?? "nil")")
                                
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
                                    print("🔧 macOS OpenVPN Plugin: VPN tunnel started successfully")
                                    completion(nil)
                                } catch {
                                    print("🔧 macOS OpenVPN Plugin: Failed to start VPN tunnel: \(error)")
                                    print("🔧 macOS OpenVPN Plugin: Error details: \(error.localizedDescription)")
                                    completion(error)
                                }
                            } catch let error {
                                print("🔧 macOS OpenVPN Plugin: Failed to start VPN tunnel: \(error)")
                                completion(error)
                            }
                        })
                    } else {
                        print("🔧 macOS OpenVPN Plugin: Failed to save preferences: \(error?.localizedDescription ?? "unknown error")")
                        completion(error)
                    }
                })
            } else {
                print("🔧 macOS OpenVPN Plugin: Failed to load preferences: \(error?.localizedDescription ?? "unknown error")")
                completion(error)
            }
        }
    }
    
    func stopVPN() {
        print("🔧 macOS OpenVPN Plugin: Stopping VPN tunnel...")
        
        // Ensure we're on the main queue for UI updates
        DispatchQueue.main.async {
            self.providerManager.connection.stopVPNTunnel()
            print("🔧 macOS OpenVPN Plugin: VPN tunnel stop requested")
            
            // Force a status update to ensure the UI gets notified
            if let stage = self.stage {
                print("🔧 macOS OpenVPN Plugin: Forcing disconnecting status update")
                stage("disconnecting")
            }
        }
    }
    
    func getTraffictStats() {
        if let sharedDefaults = UserDefaults.init(suiteName: groupIdentifier) {
            let formatter = DateFormatter()
            formatter.dateFormat = "yyyy-MM-dd HH:mm:ss"
            
            let connectedDate = sharedDefaults.object(forKey: "connected_date") as? Date ?? Date()
            let bytes_in = sharedDefaults.object(forKey: "bytes_in") as? String ?? "0"
            let bytes_out = sharedDefaults.object(forKey: "bytes_out") as? String ?? "0"
            let packets_in = sharedDefaults.object(forKey: "packets_in") as? String ?? "0"
            let packets_out = sharedDefaults.object(forKey: "packets_out") as? String ?? "0"
            
            let connectionUpdate = "\(formatter.string(from: connectedDate))_\(packets_in)_\(packets_out)_\(bytes_in)_\(bytes_out)"
            sharedDefaults.set(connectionUpdate, forKey: "connectionUpdate")
        }
    }
} 