# Windows OpenVPN Flutter Setup Guide

## Overview

This guide helps you set up the OpenVPN Flutter plugin for Windows with proper file bundling and driver support.

## Required Files

### 1. OpenVPN Binaries (bundle/bin/)
Place these files in the `windows/bundle/bin/` directory:

- `openvpn.exe` - OpenVPN executable with WinTun support
- `wintun.dll` - WinTun driver library (preferred, high performance)
- `libcrypto_3_x64.dll` - OpenSSL crypto library 
- `libssl_3_x64.dll` - OpenSSL SSL library

### 2. TAP Driver Files (bundle/drivers/tap-windows/) - Optional Fallback
For maximum compatibility, include TAP-Windows driver files:

- `tapinstall.exe` - TAP driver installer utility
- `OemVista.inf` - TAP driver information file
- `tap0901.sys` - TAP driver binary
- Additional driver files as needed

## File Structure

```
your_flutter_app/
├── windows/
│   ├── runner/
│   │   ├── bin/                    # Files will be copied here during build
│   │   │   ├── openvpn.exe
│   │   │   ├── wintun.dll
│   │   │   ├── libcrypto_3_x64.dll
│   │   │   └── libssl_3_x64.dll
│   │   └── drivers/                # Optional TAP driver files
│   └── flutter/
└── pubspec.yaml
```

## Build Configuration

The plugin automatically copies bundled files during build. Ensure your `windows/CMakeLists.txt` includes:

```cmake
# Files are automatically installed by the plugin
```

## Driver Selection Strategy

1. **WinTun (Preferred)**: High performance, uses bundled `wintun.dll`
2. **TAP-Windows (Fallback)**: Compatibility mode, requires driver installation

## Usage in Flutter App

### Basic Initialization

```dart
import 'package:openvpn_flutter/openvpn_flutter.dart';

late OpenVPN vpn;

@override
void initState() {
  super.initState();
  
  vpn = OpenVPN(
    onVpnStatusChanged: (data) {
      print('VPN Status: $data');
    },
    onVpnStageChanged: (stage, rawStage) {
      print('VPN Stage: $stage');
    },
  );
  
  // Initialize VPN engine
  _initializeVPN();
}

Future<void> _initializeVPN() async {
  try {
    await vpn.initialize();
    print('VPN initialized successfully');
  } catch (e) {
    print('VPN initialization failed: $e');
    // Handle initialization error
  }
}
```

### Connect to VPN

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
# Your OpenVPN configuration
""";

    await vpn.connect(
      config,
      "My VPN Connection",
      username: "your_username",    // Optional
      password: "your_password",    // Optional
    );
  } catch (e) {
    print('Connection failed: $e');
  }
}
```

## Troubleshooting

### Common Issues

1. **"OpenVPN executable not found"**
   - Ensure `openvpn.exe` is in the correct bundle location
   - Check build output for file copying errors

2. **"Failed to initialize VPN driver"**
   - Verify `wintun.dll` is present and not corrupted
   - For TAP fallback: check if TAP-Windows is installed
   - Run application as administrator if needed

3. **"Connection failed"**
   - Verify OpenVPN configuration is valid
   - Check Windows Firewall settings
   - Ensure no other VPN is running

### Driver Issues

**WinTun Issues:**
- Download latest WinTun from: https://www.wintun.net/
- Ensure 64-bit version matches your application architecture

**TAP-Windows Issues:**
- Install TAP-Windows from OpenVPN website
- Or use bundled installer with administrator privileges

### Debugging

Enable verbose logging in your Flutter app:
```dart
// Check VPN status regularly
Timer.periodic(Duration(seconds: 2), (timer) async {
  VPNStage stage = await vpn.stage();
  VpnStatus status = await vpn.status();
  print('Stage: $stage, Status: $status');
});
```

## Security Notes

- WinTun provides better security isolation than TAP-Windows
- Ensure OpenVPN configuration includes proper security settings
- Consider certificate-based authentication for production use

## Performance

- WinTun: 800+ Mbps throughput typical
- TAP-Windows: ~200 Mbps throughput typical
- Plugin automatically selects best available driver

## License Requirements

- **WinTun**: MIT License (redistributable)
- **OpenVPN**: GPL v2 (check licensing requirements)
- **TAP-Windows**: GPL v2 (check licensing requirements)

Ensure your application complies with the respective licenses when redistributing these components. 