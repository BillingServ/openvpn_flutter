import Flutter
import UIKit
import NetworkExtension

public class SwiftOpenVPNFlutterPlugin: NSObject, FlutterPlugin {
    private static var utils : VPNUtils?
    
    private static var EVENT_CHANNEL_VPN_STAGE = "id.laskarmedia.openvpn_flutter/vpnstage"
    private static var METHOD_CHANNEL_VPN_CONTROL = "id.laskarmedia.openvpn_flutter/vpncontrol"
     
    public static var stage: FlutterEventSink?
    private var initialized : Bool = false
    
    public static func register(with registrar: FlutterPluginRegistrar) {
        let instance = SwiftOpenVPNFlutterPlugin()
        instance.onRegister(registrar)
    }
    
    public func onRegister(_ registrar: FlutterPluginRegistrar){
        let vpnControlM = FlutterMethodChannel(name: SwiftOpenVPNFlutterPlugin.METHOD_CHANNEL_VPN_CONTROL, binaryMessenger: registrar.messenger())
        let vpnStageE = FlutterEventChannel(name: SwiftOpenVPNFlutterPlugin.EVENT_CHANNEL_VPN_STAGE, binaryMessenger: registrar.messenger())
        
        vpnStageE.setStreamHandler(StageHandler())
        vpnControlM.setMethodCallHandler({(call: FlutterMethodCall, result: @escaping FlutterResult) -> Void in
            switch call.method {
            case "status":
                if let utils = SwiftOpenVPNFlutterPlugin.utils {
                    utils.getTraffictStats()
                    result(UserDefaults.init(suiteName: utils.groupIdentifier)?.string(forKey: "connectionUpdate"))
                } else {
                    result("disconnected")
                }
                break;
            case "stage":
                if let status = SwiftOpenVPNFlutterPlugin.utils?.currentStatus() {
                    result(status)
                } else {
                    result(FlutterError(code: "VPN_IDENTITY_UNKNOWN",
                        message: "VPN provider identity is not initialized", details: nil))
                }
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
                // Lazy initialization - only create VPNUtils when actually needed
                if SwiftOpenVPNFlutterPlugin.utils == nil {
                    SwiftOpenVPNFlutterPlugin.utils = VPNUtils()
                    SwiftOpenVPNFlutterPlugin.utils?.stage = SwiftOpenVPNFlutterPlugin.stage
                }
                
                SwiftOpenVPNFlutterPlugin.utils!.groupIdentifier = groupIdentifier
                SwiftOpenVPNFlutterPlugin.utils!.localizedDescription = localizedDescription
                SwiftOpenVPNFlutterPlugin.utils!.providerBundleIdentifier = providerBundleIdentifier
                SwiftOpenVPNFlutterPlugin.utils!.loadProviderManager{(err:Error?) in
                    if err == nil{
                        result(SwiftOpenVPNFlutterPlugin.utils!.currentStatus())
                    }else{
                        result(FlutterError(code: "-4", message: err?.localizedDescription, details: err?.localizedDescription));
                    }
                }
                self.initialized = true
                break;
            case "disconnect":
                SwiftOpenVPNFlutterPlugin.utils?.stopVPN()
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
                
                SwiftOpenVPNFlutterPlugin.utils?.configureVPN(config: config, username: username, password: password, completion: {(success:Error?) -> Void in
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
                self.initialized = false
            default:
                break;
            }
        })
    }
    
    
    class StageHandler: NSObject, FlutterStreamHandler {
        func onListen(withArguments arguments: Any?, eventSink events: @escaping FlutterEventSink) -> FlutterError? {
            SwiftOpenVPNFlutterPlugin.stage = events
            SwiftOpenVPNFlutterPlugin.utils?.stage = events
            return nil
        }
        
        func onCancel(withArguments arguments: Any?) -> FlutterError? {
            SwiftOpenVPNFlutterPlugin.stage = nil
            SwiftOpenVPNFlutterPlugin.utils?.stage = nil
            return nil
        }
    }
    
    
}


@available(iOS 9.0, *)
class VPNUtils {
    private var operationGeneration: UInt64 = 0
    private var pendingStart: ((Error?) -> Void)?
    var providerManager: NETunnelProviderManager!
    var providerBundleIdentifier : String? {
        didSet {
            guard oldValue != providerBundleIdentifier else { return }
            operationGeneration &+= 1
            providerManager = nil
            let cancelledStart = pendingStart
            pendingStart = nil
            cancelledStart?(NSError(domain: "OpenVPNFlutter", code: 4,
                userInfo: [NSLocalizedDescriptionKey: "VPN provider identity changed"]))
        }
    }
    var localizedDescription : String?
    var groupIdentifier : String?
    var stage : FlutterEventSink!
    var vpnStageObserver : NSObjectProtocol?
    
    func ownsManager(_ manager: NETunnelProviderManager) -> Bool {
        guard let identifier = providerBundleIdentifier, !identifier.isEmpty,
              let configuration = manager.protocolConfiguration as? NETunnelProviderProtocol else { return false }
        return configuration.providerBundleIdentifier == identifier
    }

    func ownsConnection(_ connection: NEVPNConnection) -> Bool {
        guard let manager = providerManager, ownsManager(manager) else { return false }
        return connection === manager.connection
    }

    func loadProviderManager(completion:@escaping (_ error : Error?) -> Void)  {
        let generation = operationGeneration
        NETunnelProviderManager.loadAllFromPreferences { (managers, error)  in
            guard self.operationGeneration == generation else { completion(nil); return }
            if error == nil {
                self.providerManager = managers?.first(where: self.ownsManager) ?? NETunnelProviderManager()
                completion(nil)
            } else {
                completion(error)
            }
        }
    }
    
    func onVpnStatusChanged(notification : NEVPNStatus) {
        switch notification {
        case NEVPNStatus.connected:
            stage?("connected")
            break;
        case NEVPNStatus.connecting:
            stage?("connecting")
            break;
        case NEVPNStatus.disconnected:
            stage?("disconnected")
            break;
        case NEVPNStatus.disconnecting:
            stage?("disconnecting")
            break;
        case NEVPNStatus.invalid:
            stage?("invalid")
            break;
        case NEVPNStatus.reasserting:
            stage?("reasserting")
            break;
        default:
            stage?("null")
            break;
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
            return "";
        }
    }
    
    func currentStatus() -> String? {
        guard let identifier = providerBundleIdentifier, !identifier.isEmpty else { return nil }
        if let manager = self.providerManager, ownsManager(manager) {
            return onVpnStatusChangedString(notification: manager.connection.status)}
        else{
            return "disconnected"
        }
    }
    
    func configureVPN(config: String?, username : String?,password : String?,completion:@escaping (_ error : Error?) -> Void) {
        operationGeneration &+= 1
        let generation = operationGeneration
        let previousStart = pendingStart
        pendingStart = completion
        previousStart?(NSError(domain: "OpenVPNFlutter", code: 1,
            userInfo: [NSLocalizedDescriptionKey: "Superseded by a newer start"]))
        let complete: (Error?) -> Void = { [weak self] error in
            guard let self = self, self.operationGeneration == generation else { return }
            let callback = self.pendingStart
            self.pendingStart = nil
            callback?(error)
        }
        guard let manager = providerManager else {
            complete(NSError(domain: "OpenVPNFlutter", code: 2,
                userInfo: [NSLocalizedDescriptionKey: "VPN manager is not initialized"]))
            return
        }
        let configData = config
        manager.loadFromPreferences { error in
            guard self.operationGeneration == generation else { return }
            if error == nil {
                let tunnelProtocol = NETunnelProviderProtocol()
                tunnelProtocol.serverAddress = ""
                tunnelProtocol.providerBundleIdentifier = self.providerBundleIdentifier
                let nullData = "".data(using: .utf8)
                tunnelProtocol.providerConfiguration = [
                    "config": configData?.data(using: .utf8) ?? nullData!,
                    "groupIdentifier": self.groupIdentifier?.data(using: .utf8) ?? nullData!,
                    "username" : username?.data(using: .utf8) ?? nullData!,
                    "password" : password?.data(using: .utf8) ?? nullData!
                ]
                tunnelProtocol.disconnectOnSleep = false
                self.providerManager.protocolConfiguration = tunnelProtocol
                self.providerManager.localizedDescription = self.localizedDescription // the title of the VPN profile which will appear on Settings
                self.providerManager.isEnabled = true
                self.providerManager.saveToPreferences(completionHandler: { (error) in
                    guard self.operationGeneration == generation else { return }
                    if error == nil  {
                        self.providerManager.loadFromPreferences(completionHandler: { (error) in
                            guard self.operationGeneration == generation else { return }
                            if error != nil {
                                complete(error);
                                return;
                            }
                            do {
                                if self.vpnStageObserver != nil {
                                    NotificationCenter.default.removeObserver(self.vpnStageObserver!,
                                                                              name: NSNotification.Name.NEVPNStatusDidChange,
                                                                              object: nil)
                                }
                                self.vpnStageObserver = NotificationCenter.default.addObserver(forName: NSNotification.Name.NEVPNStatusDidChange,
                                                                                               object: nil ,
                                                                                               queue: .main) { [weak self] notification in
                                    guard let self = self,
                                          let connection = notification.object as? NEVPNConnection,
                                          self.ownsConnection(connection) else { return }
                                    self.onVpnStatusChanged(notification: connection.status)
                                }
                                
                                if username != nil && password != nil{
                                    let options: [String : NSObject] = [
                                        "username": username! as NSString,
                                        "password": password! as NSString
                                    ]
                                    try self.providerManager.connection.startVPNTunnel(options: options)
                                }else{
                                    try self.providerManager.connection.startVPNTunnel()
                                }
                                complete(nil);
                            } catch let error {
                                complete(error)
                                self.stopVPN()
                                print("Error info: \(error)")
                            }
                        })
                    } else {
                        complete(error);
                    }
                })
            } else {
                complete(error)
            }
        }
        
        
    }
    
    func stopVPN() {
        operationGeneration &+= 1
        let cancelledStart = pendingStart
        pendingStart = nil
        cancelledStart?(NSError(domain: "OpenVPNFlutter", code: 3,
            userInfo: [NSLocalizedDescriptionKey: "Stopped"]))
        if let manager = providerManager, ownsManager(manager) {
            manager.connection.stopVPNTunnel()
        };
    }
    
    func getTraffictStats(){
        if let session = self.providerManager?.connection as? NETunnelProviderSession {
            do {
                try session.sendProviderMessage("OPENVPN_STATS".data(using: .utf8)!) {(data) in
                    //Do nothing
                }
            } catch {
            // some error
            }
        }
    }
}
