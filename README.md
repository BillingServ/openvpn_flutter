# openvpn_flutter

A Flutter plugin that allows you to connect to OpenVPN servers on multiple platforms.

## Platform Support

| Platform | Support Status | VPN Type | Requirements |
|----------|----------------|----------|--------------|
| Android  | ✅ Full | Native | OpenVPN library |
| iOS      | ✅ Full | NetworkExtension | Network Extension target |
| **macOS**    | ✅ **Full** | **NetworkExtension** | **Network Extension target** |
| **Windows**  | ✅ **Full** | **WinTun + TAP** | **Bundled drivers** |
| Linux    | ❌ Not yet | - | - |

## Features

- ✅ Connect/Disconnect from OpenVPN servers
- ✅ Real-time connection status monitoring  
- ✅ Connection statistics (bytes in/out, duration)
- ✅ Multiple platform support
- ✅ Username/password authentication
- ✅ Certificate-based authentication
- ✅ Custom configuration support

## Requirements

### All Platforms
```yaml
dependencies:
  openvpn_flutter: ^1.3.4
```

### iOS & macOS Requirements
- Xcode project with Network Extension capability
- App Group entitlement configured
- Network Extension Bundle ID
- Proper code signing certificates

### Windows Requirements
- **WinTun driver preferred** - 4x faster performance (800+ Mbps vs 200 Mbps)
- **TAP-Windows fallback** - for compatibility
- **Bundled drivers included** - no external installations required
- Administrator privileges may be required for TAP driver installation (fallback only)
- Windows 10+ recommended (Windows 8.1+ supported)

### Android Requirements
- VPN permission in AndroidManifest.xml
- Target SDK 21+

## Quick Start

### 1. Initialize the VPN Engine

```dart
import 'package:openvpn_flutter/openvpn_flutter.dart';

late OpenVPN vpn;

@override
void initState() {
  super.initState();
  
  vpn = OpenVPN(
    onVpnStatusChanged: (data) {
      print('VPN Status: ${data?.connectedOn}');
      print('Duration: ${data?.duration}');
      print('Bytes In: ${data?.byteIn}');
      print('Bytes Out: ${data?.byteOut}');
    },
    onVpnStageChanged: (stage, rawStage) {
      print('VPN Stage: $stage');
      // Stages: disconnected, connecting, connected, disconnecting
    },
  );
}
```

### 2. Platform-Specific Initialization

```dart
Future<void> initializeVPN() async {
  try {
    if (Platform.isIOS || Platform.isMacOS) {
      // Required for iOS and macOS
      await vpn.initialize(
        groupIdentifier: "group.com.yourapp.vpn",
        providerBundleIdentifier: "com.yourapp.vpn.extension",
        localizedDescription: "Your VPN App",
      );
    } else {
      // Android and Windows
      await vpn.initialize();
    }
    print('VPN initialized successfully');
  } catch (e) {
    print('VPN initialization failed: $e');
  }
}
```

### 3. Connect to VPN

```dart
Future<void> connectToVPN() async {
  try {
    String config = """
client
dev tun
proto udp
remote your-server.com 1194
resolv-retry infinite
nobind
persist-key
persist-tun
ca [inline]
cert [inline]
key [inline]
# Your OpenVPN configuration here
""";

    await vpn.connect(
      config, 
      "My VPN Connection",
      username: "your_username",    // Optional
      password: "your_password",    // Optional
      certIsRequired: false,
    );
  } catch (e) {
    print('Connection failed: $e');
  }
}
```

### 4. Monitor Connection

```dart
// Check if connected
bool isConnected = await vpn.isConnected();

// Get current status
VpnStatus status = await vpn.status();
print('Connected on: ${status.connectedOn}');
print('Duration: ${status.duration}');

// Get current stage
VPNStage stage = await vpn.stage();
print('Current stage: $stage');
```

### 5. Disconnect

```dart
Future<void> disconnectVPN() async {
  vpn.disconnect();
}
```

## Platform-Specific Setup

### Windows Setup

1. **Bundle Required Files**:
   - Include OpenVPN executable with WinTun support
   - Include `wintun.dll` for high-performance connections  
   - Include TAP-Windows drivers as fallback
   - See [Bundle Structure Guide](windows/BUNDLE_STRUCTURE.md) for details

2. **Driver Selection**:
   - **WinTun**: Preferred driver (4x faster, MIT license)
   - **TAP-Windows**: Automatic fallback for compatibility
   - Plugin automatically selects best available driver

3. **Example Windows Usage**:
```dart
// Windows automatically uses WinTun (preferred) or TAP-Windows (fallback)
await vpn.initialize(); // Sets up optimal driver automatically

// Check which driver is being used
// (Optional: for debugging/logging purposes)
await vpn.connect(openVpnConfig, "Windows VPN");
```

### macOS Setup

1. **Create Network Extension Target**:
   - Add Network Extension target to your Xcode project
   - Configure App Groups capability
   - Set proper bundle identifiers

2. **Configure Entitlements**:
```xml
<!-- macOS App Entitlements -->
<key>com.apple.security.app-sandbox</key>
<true/>
<key>com.apple.security.network.client</key>
<true/>
<key>com.apple.developer.networking.networkextension</key>
<array>
    <string>packet-tunnel-provider</string>
</array>
```

3. **Example macOS Usage**:
```dart
await vpn.initialize(
  groupIdentifier: "group.com.yourapp.vpn",
  providerBundleIdentifier: "com.yourapp.vpn.macos",
  localizedDescription: "YourApp VPN",
);
```

### iOS Setup

1. **Create Network Extension Target** (same as macOS)
2. **Configure iOS Entitlements**
3. **Example iOS Usage** (same as macOS)

### Android Setup

1. **Add Permissions**:
```xml
<!-- android/app/src/main/AndroidManifest.xml -->
<uses-permission android:name="android.permission.INTERNET" />
<uses-permission android:name="android.permission.ACCESS_NETWORK_STATE" />
```

2. **Example Android Usage**:
```dart
// Request VPN permission (Android only)
bool hasPermission = await vpn.requestPermissionAndroid();
if (hasPermission) {
  await vpn.initialize();
  await vpn.connect(config, "Android VPN");
}
```

## Advanced Features

### Connection Filtering
```dart
// Filter multiple remote servers to use just one (reduces ANR on Android)
String? filteredConfig = await OpenVPN.filteredConfig(originalConfig);
await vpn.connect(filteredConfig!, "Filtered Connection");
```

### Bypass Apps (Android Only)
```dart
await vpn.connect(
  config, 
  "VPN Connection",
  bypassPackages: [
    "com.google.android.gms",
    "com.android.chrome"
  ]
);
```

## Error Handling

```dart
try {
  await vpn.connect(config, "VPN");
} on PlatformException catch (e) {
  switch (e.code) {
    case 'invalid_config':
      print('Invalid OpenVPN configuration');
      break;
    case 'connection_failed':
      print('Failed to establish VPN connection');
      break;
    case 'permission_denied':
      print('VPN permission denied');
      break;
    default:
      print('Unknown error: ${e.message}');
  }
}
```

## Troubleshooting

### Windows Issues
- **"TAP driver not found"**: Ensure bundled TAP driver files are present and run as administrator
- **"Access denied"**: Run app as administrator for TAP driver installation
- **"Bundled files missing"**: Check that OpenVPN executable and TAP driver are bundled correctly
- **Connection fails**: Check Windows Defender/Firewall settings and TAP adapter status

### macOS Issues  
- **"Network Extension not found"**: Ensure proper bundle IDs and entitlements
- **Permission denied**: Check system VPN permissions in System Preferences

### iOS Issues
- **Same as macOS**: Network Extension configuration required

### Android Issues
- **VPN permission**: Call `requestPermissionAndroid()` first
- **ANR (App Not Responding)**: Use `filteredConfig()` to limit remote servers

## Contributing

Contributions are welcome! Please read our contributing guidelines and submit pull requests.

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
