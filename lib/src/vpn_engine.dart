import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math';
import 'package:flutter/services.dart';
import 'model/vpn_status.dart';

///Stages of vpn connections
enum VPNStage {
  prepare,
  authenticating,
  connecting,
  authentication,
  connected,
  disconnected,
  disconnecting,
  denied,
  error,
// ignore: constant_identifier_names
  wait_connection,
// ignore: constant_identifier_names
  vpn_generate_config,
// ignore: constant_identifier_names
  get_config,
// ignore: constant_identifier_names
  tcp_connect,
// ignore: constant_identifier_names
  udp_connect,
// ignore: constant_identifier_names
  assign_ip,
  resolve,
  exiting,
  unknown
}

class OpenVPN {
  ///Channel's names of _vpnStageSnapshot
  static const String _eventChannelVpnStage =
      "id.laskarmedia.openvpn_flutter/vpnstage";

  ///Channel's names of _channelControl
  static const String _methodChannelVpnControl =
      "id.laskarmedia.openvpn_flutter/vpncontrol";

  ///Method channel to invoke methods from native side
  static const MethodChannel _channelControl =
      MethodChannel(_methodChannelVpnControl);

  ///Snapshot of stream that produced by native side
  static Stream<String> _vpnStageSnapshot() =>
      const EventChannel(_eventChannelVpnStage).receiveBroadcastStream().cast();

  ///Timer to get vpnstatus as a loop
  ///
  ///I know it was bad practice, but this is the only way to avoid android status duration having long delay
  Timer? _vpnStatusTimer;

  ///To indicate the engine already initialize
  bool initialized = false;

  ///Use tempDateTime to countdown, especially on android that has delays
  DateTime? _tempDateTime;

  VPNStage? _lastStage;

  /// is a listener to see vpn status detail
  final Function(VpnStatus? data)? onVpnStatusChanged;

  /// is a listener to see what stage the connection was
  final Function(VPNStage stage, String rawStage)? onVpnStageChanged;

  /// OpenVPN's Constructions, don't forget to implement the listeners
  /// onVpnStatusChanged is a listener to see vpn status detail
  /// onVpnStageChanged is a listener to see what stage the connection was
  OpenVPN({this.onVpnStatusChanged, this.onVpnStageChanged});

  ///This function should be called before any usage of OpenVPN
  ///All params required for iOS and macOS, make sure you read the plugin's documentation
  ///
  ///
  ///providerBundleIdentfier is for your Network Extension identifier
  ///
  ///localizedDescription is for description to show in user's settings
  ///
  ///
  ///Will return latest VPNStage
  Future<void> initialize({
    String? providerBundleIdentifier,
    String? localizedDescription,
    String? groupIdentifier,
    Function(VpnStatus status)? lastStatus,
    Function(VPNStage stage)? lastStage,
  }) async {
    if (Platform.isIOS || Platform.isMacOS) {
      assert(
          groupIdentifier != null &&
              providerBundleIdentifier != null &&
              localizedDescription != null,
          "These values are required for iOS and macOS.");
    }
    onVpnStatusChanged?.call(VpnStatus.empty());
    initialized = true;
    _initializeListener();
    
    // Defer the heavy native initialization to avoid blocking startup
    return Future.microtask(() async {
      await _channelControl.invokeMethod("initialize", {
        "groupIdentifier": groupIdentifier,
        "providerBundleIdentifier": providerBundleIdentifier,
        "localizedDescription": localizedDescription,
      });
      
      // Get initial status in background
      Future.wait([
        status().then((value) => lastStatus?.call(value)),
        stage().then((value) {
          if (value == VPNStage.connected && _vpnStatusTimer == null) {
            _createTimer();
          }
          return lastStage?.call(value);
        }),
      ]);
    });
  }

  ///Connect to VPN
  ///
  ///config : Your openvpn configuration script, you can find it inside your .ovpn file
  ///
  ///name : name that will show in user's notification
  ///
  ///certIsRequired : default is false, if your config file has cert, set it to true
  ///
  ///username & password : set your username and password if your config file has auth-user-pass
  ///
  ///bypassPackages : exclude some apps to access/use the VPN Connection, it was List<String> of applications package's name (Android Only)
  Future connect(String config, String name,
      {String? username,
      String? password,
      List<String>? bypassPackages,
      bool certIsRequired = false}) {
    if (!initialized) throw ("OpenVPN need to be initialized");
    // Remove automatic addition of cert options - config should be complete
    _tempDateTime = DateTime.now();

    print('🔧 OpenVPN Plugin: About to call _channelControl.invokeMethod("connect")');
    print('🔧 OpenVPN Plugin: Config length: ${config.length}');
    print('🔧 OpenVPN Plugin: Name: $name');
    print('🔧 OpenVPN Plugin: Username: $username');
    print('🔧 OpenVPN Plugin: Password provided: ${password != null}');

    try {
      final result = _channelControl.invokeMethod("connect", {
        "config": config,
        "name": name,
        "username": username,
        "password": password,
        "bypass_packages": bypassPackages ?? []
      });
      print('🔧 OpenVPN Plugin: _channelControl.invokeMethod("connect") called successfully');
      return result;
    } on PlatformException catch (e) {
      print('❌ OpenVPN Plugin: Method channel call failed: ${e.message}');
      throw ArgumentError(e.message);
    }
  }

  ///Disconnect from VPN
  void disconnect() {
    _tempDateTime = null;
    _channelControl.invokeMethod("disconnect");
    if (_vpnStatusTimer?.isActive ?? false) {
      _vpnStatusTimer?.cancel();
      _vpnStatusTimer = null;
    }
  }

  ///Check if connected to vpn
  Future<bool> isConnected() async =>
      stage().then((value) => value == VPNStage.connected);

  ///Get latest connection stage
  Future<VPNStage> stage() async {
    String? stage = await _channelControl.invokeMethod("stage");
    return _strToStage(stage ?? "disconnected");
  }

  ///Get latest connection status
  Future<VpnStatus> status() {
    //Have to check if user already connected to get real data
    return stage().then((value) async {
      var status = VpnStatus.empty();
      if (value == VPNStage.connected) {
        status = await _channelControl.invokeMethod("status").then((value) {
          if (value == null) return VpnStatus.empty();

          if (Platform.isIOS || Platform.isMacOS) {
            var splitted = value.split("_");
            var connectedOn = DateTime.tryParse(splitted[0]);
            if (connectedOn == null) return VpnStatus.empty();
            return VpnStatus(
              connectedOn: connectedOn,
              duration: _duration(DateTime.now().difference(connectedOn).abs()),
              packetsIn: splitted[1],
              packetsOut: splitted[2],
              byteIn: splitted[3],
              byteOut: splitted[4],
            );
          } else if (Platform.isAndroid) {
            var data = jsonDecode(value);
            var connectedOn =
                DateTime.tryParse(data["connected_on"].toString()) ??
                    _tempDateTime ??
                    DateTime.now();
            String byteIn =
                data["byte_in"] != null ? data["byte_in"].toString() : "0";
            String byteOut =
                data["byte_out"] != null ? data["byte_out"].toString() : "0";
            if (byteIn.trim().isEmpty) byteIn = "0";
            if (byteOut.trim().isEmpty) byteOut = "0";
            return VpnStatus(
              connectedOn: connectedOn,
              duration: _duration(DateTime.now().difference(connectedOn).abs()),
              byteIn: byteIn,
              byteOut: byteOut,
              packetsIn: byteIn,
              packetsOut: byteOut,
            );
          } else if (Platform.isWindows || Platform.isLinux) {
            // Desktop platforms - return mock data or parse JSON if available
            try {
              var data = jsonDecode(value);
              var connectedOn = DateTime.tryParse(data["connected_on"]?.toString() ?? "") ?? 
                               _tempDateTime ?? 
                               DateTime.now();
              String byteIn = data["byte_in"]?.toString() ?? "0";
              String byteOut = data["byte_out"]?.toString() ?? "0";
              if (byteIn.trim().isEmpty) byteIn = "0";
              if (byteOut.trim().isEmpty) byteOut = "0";
              return VpnStatus(
                connectedOn: connectedOn,
                duration: _duration(DateTime.now().difference(connectedOn).abs()),
                byteIn: byteIn,
                byteOut: byteOut,
                packetsIn: byteIn,
                packetsOut: byteOut,
              );
            } catch (e) {
              // Fallback to empty status for desktop platforms
              return VpnStatus.empty();
            }
          } else {
            throw Exception("OpenVPN not supported on this platform");
          }
        });
      }
      return status;
    });
  }

  ///Force status check (useful for debugging on macOS)
  Future<void> forceStatusCheck() async {
    if (Platform.isMacOS) {
      try {
        await _channelControl.invokeMethod("forceStatusCheck");
        print('🔧 OpenVPN Plugin: Force status check triggered');
      } catch (e) {
        print('❌ OpenVPN Plugin: Force status check failed: $e');
      }
    }
  }

  ///Request android permission (Return true if already granted)
  Future<bool> requestPermissionAndroid() async {
    return _channelControl
        .invokeMethod("request_permission")
        .then((value) => value ?? false);
  }

  ///Sometimes config script has too many Remotes, it cause ANR in several devices,
  ///This happened because the plugin check every remote and somehow affected the UI to freeze
  ///
  ///Use this function if you wanted to force user to use 1 remote by randomize the remotes provided
  static Future<String?> filteredConfig(String? config) async {
    List<String> remotes = [];
    List<String> output = [];
    if (config == null) return null;
    var raw = config.split("\n");

    for (var item in raw) {
      if (item.trim().toLowerCase().startsWith("remote ")) {
        if (!output.contains("REMOTE_HERE")) {
          output.add("REMOTE_HERE");
        }
        remotes.add(item);
      } else {
        output.add(item);
      }
    }
    String fastestServer = remotes[Random().nextInt(remotes.length - 1)];
    int indexRemote = output.indexWhere((element) => element == "REMOTE_HERE");
    output.removeWhere((element) => element == "REMOTE_HERE");
    output.insert(indexRemote, fastestServer);
    return output.join("\n");
  }

  ///Convert duration that produced by native side as Connection Time
  String _duration(Duration duration) {
    String twoDigits(int n) => n.toString().padLeft(2, "0");
    String twoDigitMinutes = twoDigits(duration.inMinutes.remainder(60));
    String twoDigitSeconds = twoDigits(duration.inSeconds.remainder(60));
    return "${twoDigits(duration.inHours)}:$twoDigitMinutes:$twoDigitSeconds";
  }

  ///Private function to convert String to VPNStage
  static VPNStage _strToStage(String? stage) {
    if (stage == null ||
        stage.trim().isEmpty ||
        stage.trim() == "idle" ||
        stage.trim() == "invalid") {
      return VPNStage.disconnected;
    }
    var indexStage = VPNStage.values.indexWhere((element) => element
        .toString()
        .trim()
        .toLowerCase()
        .contains(stage.toString().trim().toLowerCase()));
    if (indexStage >= 0) return VPNStage.values[indexStage];
    return VPNStage.unknown;
  }

  ///Initialize listener, called when you start connection and stoped while
  void _initializeListener() {
    print('🔧 OpenVPN Plugin: Initializing listener...');
    _vpnStageSnapshot().listen((event) {
      print('🔧 OpenVPN Plugin: Received stage event: $event');
      var vpnStage = _strToStage(event);
      print('🔧 OpenVPN Plugin: Converted to VPNStage: $vpnStage');
      
      if (vpnStage != _lastStage) {
        print('🔧 OpenVPN Plugin: Stage changed from $_lastStage to $vpnStage');
        onVpnStageChanged?.call(vpnStage, event);
        _lastStage = vpnStage;
      }
      
      if (vpnStage != VPNStage.disconnected) {
        if (Platform.isAndroid) {
          _createTimer();
        } else if ((Platform.isIOS || Platform.isMacOS) && vpnStage == VPNStage.connected) {
          print('🔧 OpenVPN Plugin: Creating timer for iOS/macOS connected state');
          _createTimer();
        } else if ((Platform.isWindows || Platform.isLinux) && vpnStage == VPNStage.connected) {
          _createTimer();
        }
      } else {
        print('🔧 OpenVPN Plugin: Disconnected state, cancelling timer');
        _vpnStatusTimer?.cancel();
      }
    }, onError: (error) {
      print('❌ OpenVPN Plugin: Error in stage listener: $error');
    });
  }

  ///Create timer to invoke status
  void _createTimer() {
    if (_vpnStatusTimer != null) {
      print('🔧 OpenVPN Plugin: Cancelling existing timer');
      _vpnStatusTimer!.cancel();
      _vpnStatusTimer = null;
    }
    
    print('🔧 OpenVPN Plugin: Creating new status timer');
    _vpnStatusTimer ??=
        Timer.periodic(const Duration(milliseconds: 250), (timer) async {
      try {
        final vpnStatus = await status();
        print('🔧 OpenVPN Plugin: Timer status update: ${vpnStatus.toJson()}');
        onVpnStatusChanged?.call(vpnStatus);
      } catch (e) {
        print('❌ OpenVPN Plugin: Error getting status in timer: $e');
      }
    });
  }
}
