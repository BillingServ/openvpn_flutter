Run from the plugin repository root:

```sh
python3 windows/tests/vpn_manager_ownership_regression.py
python3 windows/tests/wintun_api_regression.py
```

These portable tests compile actual current native methods with C++17 clang++, substitute Win32 process and network APIs, and use temporary files for the OpenVPN log. They do not alter host adapters or establish a VPN. Actual Windows SDK compilation, installed-driver behavior, and end-to-end routing still require Windows validation.

The manager creates a separate adapter for each session and passes its exact GUID through OpenVPN `--dev-node`. TAP-Windows creation requires bundled `tapctl.exe`; WinTun also supports its DLL creation fallback. A live child and an UP adapter are insufficient: the fresh per-session log must contain the exact successful initialization event. Restart, initialization errors, child exit, and loss/replacement of the owned adapter revoke connected status and counters.

Primary API references:

- [OpenVPN 2.6 device binding](https://github.com/OpenVPN/openvpn/blob/release/2.6/doc/man-sections/vpn-network-options.rst)
- [OpenVPN initialization result](https://github.com/OpenVPN/openvpn/blob/release/2.6/src/openvpn/init.c)
- [tapctl create/delete contract](https://github.com/OpenVPN/openvpn/blob/release/2.6/src/tapctl/main.c)
- [Wintun adapter ownership and void close API](https://git.zx2c4.com/wintun/tree/api/wintun.h)
