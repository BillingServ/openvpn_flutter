import 'dart:io';

import 'package:flutter/material.dart';
import 'dart:async';

import 'package:openvpn_flutter/openvpn_flutter.dart';

void main() {
  runApp(const MyApp());
}

class MyApp extends StatefulWidget {
  const MyApp({Key? key}) : super(key: key);

  @override
  State<MyApp> createState() => _MyAppState();
}

class _MyAppState extends State<MyApp> {
  late OpenVPN engine;
  VpnStatus? status;
  String? stage;
  bool _granted = false;
  bool _initialized = false;
  String _errorMessage = '';
  
  @override
  void initState() {
    super.initState();
    
    engine = OpenVPN(
      onVpnStatusChanged: (data) {
        setState(() {
          status = data;
        });
      },
      onVpnStageChanged: (data, raw) {
        setState(() {
          stage = raw;
        });
      },
    );

    _initializeVPN();
  }

  Future<void> _initializeVPN() async {
    try {
      if (Platform.isIOS || Platform.isMacOS) {
        await engine.initialize(
          groupIdentifier: "group.com.laskarmedia.vpn",
          providerBundleIdentifier: "id.laskarmedia.openvpnFlutterExample.VPNExtension",
          localizedDescription: "VPN by Nizwar",
          lastStage: (stage) {
            setState(() {
              this.stage = stage.name;
            });
          },
          lastStatus: (status) {
            setState(() {
              this.status = status;
            });
          },
        );
      } else {
        // Windows, Android, Linux
        await engine.initialize(
          lastStage: (stage) {
            setState(() {
              this.stage = stage.name;
            });
          },
          lastStatus: (status) {
            setState(() {
              this.status = status;
            });
          },
        );
      }
      
      setState(() {
        _initialized = true;
        _errorMessage = '';
      });
      
      print('VPN initialized successfully on ${Platform.operatingSystem}');
    } catch (e) {
      setState(() {
        _initialized = false;
        _errorMessage = 'Initialization failed: $e';
      });
      
      print('VPN initialization failed: $e');
    }
  }

  Future<void> initPlatformState() async {
    if (!_initialized) {
      setState(() {
        _errorMessage = 'VPN not initialized. Please restart the app.';
      });
      return;
    }
    
    try {
      await engine.connect(
        config,
        "USA",
        username: defaultVpnUsername,
        password: defaultVpnPassword,
        certIsRequired: true,
      );
    } catch (e) {
      setState(() {
        _errorMessage = 'Connection failed: $e';
      });
      print('Connection failed: $e');
    }
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      home: Scaffold(
        appBar: AppBar(
          title: const Text('OpenVPN Flutter Example'),
        ),
        body: Center(
          child: Padding(
            padding: const EdgeInsets.all(16.0),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                if (!_initialized)
                  Card(
                    color: Colors.orange[100],
                    child: Padding(
                      padding: const EdgeInsets.all(16.0),
                      child: Column(
                        children: [
                          const Icon(Icons.warning, color: Colors.orange),
                          const SizedBox(height: 8),
                          const Text('VPN Not Initialized'),
                          if (_errorMessage.isNotEmpty) ...[
                            const SizedBox(height: 8),
                            Text(_errorMessage, style: const TextStyle(fontSize: 12)),
                          ],
                          const SizedBox(height: 8),
                          ElevatedButton(
                            onPressed: _initializeVPN,
                            child: const Text('Retry Initialize'),
                          ),
                        ],
                      ),
                    ),
                  ),
                
                if (_initialized) ...[
                  Card(
                    child: Padding(
                      padding: const EdgeInsets.all(16.0),
                      child: Column(
                        children: [
                          Text(
                            'Platform: ${Platform.operatingSystem}',
                            style: const TextStyle(fontWeight: FontWeight.bold),
                          ),
                          const SizedBox(height: 8),
                          Text(
                            'Stage: ${stage?.toString() ?? VPNStage.disconnected.toString()}',
                          ),
                          if (status != null) ...[
                            const SizedBox(height: 8),
                            Text('Status: ${status!.toJson().toString()}'),
                          ],
                        ],
                      ),
                    ),
                  ),
                  
                  const SizedBox(height: 16),
                  
                  Row(
                    mainAxisAlignment: MainAxisAlignment.spaceEvenly,
                    children: [
                      ElevatedButton(
                        onPressed: initPlatformState,
                        child: const Text("Connect"),
                      ),
                      ElevatedButton(
                        onPressed: () {
                          engine.disconnect();
                          setState(() {
                            _errorMessage = '';
                          });
                        },
                        style: ElevatedButton.styleFrom(
                          backgroundColor: Colors.red,
                          foregroundColor: Colors.white,
                        ),
                        child: const Text("Disconnect"),
                      ),
                    ],
                  ),
                ],
                
                if (Platform.isAndroid) ...[
                  const SizedBox(height: 16),
                  TextButton(
                    child: Text(_granted ? "Permission Granted" : "Request Permission"),
                    onPressed: () {
                      engine.requestPermissionAndroid().then((value) {
                        setState(() {
                          _granted = value;
                        });
                      });
                    },
                  ),
                ],
                
                if (_errorMessage.isNotEmpty && _initialized) ...[
                  const SizedBox(height: 16),
                  Card(
                    color: Colors.red[100],
                    child: Padding(
                      padding: const EdgeInsets.all(16.0),
                      child: Column(
                        children: [
                          const Icon(Icons.error, color: Colors.red),
                          const SizedBox(height: 8),
                          Text(
                            _errorMessage,
                            style: const TextStyle(color: Colors.red),
                            textAlign: TextAlign.center,
                          ),
                          const SizedBox(height: 8),
                          TextButton(
                            onPressed: () {
                              setState(() {
                                _errorMessage = '';
                              });
                            },
                            child: const Text('Clear Error'),
                          ),
                        ],
                      ),
                    ),
                  ),
                ],
              ],
            ),
          ),
        ),
      ),
    );
  }
}

const String defaultVpnUsername = "";
const String defaultVpnPassword = "";

String get config => "HERE IS YOUR OVPN SCRIPT";
