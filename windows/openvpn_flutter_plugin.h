#ifndef FLUTTER_PLUGIN_OPENVPN_FLUTTER_PLUGIN_H_
#define FLUTTER_PLUGIN_OPENVPN_FLUTTER_PLUGIN_H_

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/event_channel.h>

#include <memory>

namespace openvpn_flutter {

class OpenVPNFlutterPlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows *registrar);

  OpenVPNFlutterPlugin(flutter::PluginRegistrarWindows *registrar);

  virtual ~OpenVPNFlutterPlugin();

  // Disallow copy and assign.
  OpenVPNFlutterPlugin(const OpenVPNFlutterPlugin&) = delete;
  OpenVPNFlutterPlugin& operator=(const OpenVPNFlutterPlugin&) = delete;

 private:
  // Called when a method is called on this plugin's channel from Dart.
  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

  // Event stream handler for VPN stage changes
  std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> event_sink_;
  
  // Plugin registrar
  flutter::PluginRegistrarWindows *registrar_;
};

}  // namespace openvpn_flutter

#endif  // FLUTTER_PLUGIN_OPENVPN_FLUTTER_PLUGIN_H_ 