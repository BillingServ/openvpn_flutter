#include "openvpn_flutter_plugin.h"
#include "vpn_manager.h"

// This must be included before many other Windows headers.
#include <windows.h>

// For getPlatformVersion; remove unless needed for your plugin implementation.
#include <VersionHelpers.h>

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <flutter/event_channel.h>
#include <flutter/event_stream_handler_functions.h>

#include <memory>
#include <sstream>

#include "include/openvpn_flutter/openvpn_flutter_plugin_c_api.h"

namespace openvpn_flutter {

// Global VPN manager instance
static std::unique_ptr<VPNManager> vpnManager = std::make_unique<VPNManager>();

// Static method to register with the registrar
void OpenVPNFlutterPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows *registrar) {
  auto channel =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          registrar->messenger(), "id.laskarmedia.openvpn_flutter/vpncontrol",
          &flutter::StandardMethodCodec::GetInstance());

  auto event_channel =
      std::make_unique<flutter::EventChannel<flutter::EncodableValue>>(
          registrar->messenger(), "id.laskarmedia.openvpn_flutter/vpnstage",
          &flutter::StandardMethodCodec::GetInstance());

  auto plugin = std::make_unique<OpenVPNFlutterPlugin>(registrar);

  channel->SetMethodCallHandler(
      [plugin_pointer = plugin.get()](const auto &call, auto result) {
        plugin_pointer->HandleMethodCall(call, std::move(result));
      });

  // Set up event stream handler
  auto stream_handler = std::make_unique<flutter::StreamHandlerFunctions<flutter::EncodableValue>>(
      [plugin_pointer = plugin.get()](
          const flutter::EncodableValue* arguments,
          std::unique_ptr<flutter::EventSink<flutter::EncodableValue>>&& events)
          -> std::unique_ptr<flutter::StreamHandlerError<flutter::EncodableValue>> {
        plugin_pointer->event_sink_ = std::move(events);
        vpnManager->setEventSink(plugin_pointer->event_sink_.get());
        return nullptr;
      },
      [plugin_pointer = plugin.get()](const flutter::EncodableValue* arguments)
          -> std::unique_ptr<flutter::StreamHandlerError<flutter::EncodableValue>> {
        plugin_pointer->event_sink_.reset();
        vpnManager->setEventSink(nullptr);
        return nullptr;
      });

  event_channel->SetStreamHandler(std::move(stream_handler));

  registrar->AddPlugin(std::move(plugin));
}

OpenVPNFlutterPlugin::OpenVPNFlutterPlugin(flutter::PluginRegistrarWindows *registrar)
    : registrar_(registrar) {}

OpenVPNFlutterPlugin::~OpenVPNFlutterPlugin() {}

void OpenVPNFlutterPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue> &method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  
  const auto method_name = method_call.method_name();

  if (method_name.compare("initialize") == 0) {
    // Windows OpenVPN initialization with driver setup
    std::cout << "Initializing OpenVPN Flutter plugin for Windows..." << std::endl;
    
    // First try to initialize the driver system
    bool driverReady = vpnManager->initializeDriver();
    
    if (driverReady) {
      std::cout << "VPN driver initialized successfully" << std::endl;
      if (event_sink_) {
        event_sink_->Success(flutter::EncodableValue("disconnected"));
      }
      result->Success(flutter::EncodableValue("disconnected"));
    } else {
      std::cerr << "Failed to initialize VPN driver" << std::endl;
      result->Error("initialization_failed", 
                   "Failed to initialize VPN driver. Please ensure:\n"
                   "1. wintun.dll is present in the application directory\n"
                   "2. TAP-Windows drivers are installed (fallback)\n"
                   "3. Application has sufficient privileges\n"
                   "4. Bundled OpenVPN files are present");
    }
    
  } else if (method_name.compare("connect") == 0) {
    // Windows OpenVPN connection - Real implementation using VPNManager
    std::cout << "Starting VPN connection..." << std::endl;
    
    const auto* arguments = std::get_if<flutter::EncodableMap>(method_call.arguments());
    if (!arguments) {
      result->Error("invalid_arguments", "Invalid arguments provided");
      return;
    }
    
    // Extract parameters from arguments
    std::string config, username, password, name;
    
    auto config_it = arguments->find(flutter::EncodableValue("config"));
    if (config_it != arguments->end()) {
      if (const auto* config_str = std::get_if<std::string>(&config_it->second)) {
        config = *config_str;
      }
    }
    
    auto name_it = arguments->find(flutter::EncodableValue("name"));
    if (name_it != arguments->end()) {
      if (const auto* name_str = std::get_if<std::string>(&name_it->second)) {
        name = *name_str;
      }
    }
    
    auto username_it = arguments->find(flutter::EncodableValue("username"));
    if (username_it != arguments->end()) {
      if (const auto* username_str = std::get_if<std::string>(&username_it->second)) {
        username = *username_str;
      }
    }
    
    auto password_it = arguments->find(flutter::EncodableValue("password"));
    if (password_it != arguments->end()) {
      if (const auto* password_str = std::get_if<std::string>(&password_it->second)) {
        password = *password_str;
      }
    }
    
    if (config.empty()) {
      result->Error("invalid_config", "OpenVPN configuration is required");
      return;
    }
    
    std::cout << "Connecting to VPN: " << name << std::endl;
    
    // Start VPN connection using VPNManager
    if (vpnManager->startVPN(config, username, password)) {
      std::cout << "VPN connection initiated successfully" << std::endl;
      result->Success();
    } else {
      std::cerr << "Failed to start VPN connection" << std::endl;
      std::string errorMsg = "Failed to start OpenVPN connection. Possible causes:\n"
                            "1. OpenVPN executable not found (openvpn.exe)\n"
                            "2. VPN driver not available (WinTun or TAP-Windows)\n"
                            "3. Invalid configuration file\n"
                            "4. Insufficient privileges\n"
                            "5. Another VPN connection is active";
      result->Error("connection_failed", errorMsg);
    }
    
  } else if (method_name.compare("disconnect") == 0) {
    // Windows OpenVPN disconnection using VPNManager
    vpnManager->stopVPN();
    result->Success();
    
  } else if (method_name.compare("status") == 0) {
    // Return connection status from VPNManager
    std::string statusJson = vpnManager->getConnectionStats();
    result->Success(flutter::EncodableValue(statusJson));
    
  } else if (method_name.compare("stage") == 0) {
    // Return current stage from VPNManager
    std::string currentStage = vpnManager->getStatus();
    result->Success(flutter::EncodableValue(currentStage));
    
  } else if (method_name.compare("request_permission") == 0) {
    // Windows doesn't require VPN permissions like Android
    result->Success(flutter::EncodableValue(true));
    
  } else {
    result->NotImplemented();
  }
}

}  // namespace openvpn_flutter

// C API implementation
void OpenVPNFlutterPluginCApiRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  openvpn_flutter::OpenVPNFlutterPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
} 