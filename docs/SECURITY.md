# Security and privacy

The root hook opens an **unauthenticated root shell** on TCP 5777 and writable
TFTP on UDP 1069; API TCP 8130 is not an authenticated multi-user boundary.
The portable hook restricts these ports to IPv4 loopback/private/link-local
source ranges. That is not protection from another untrusted LAN device.
Use a trusted isolated VLAN/LAN, no Internet port forwarding, and router/firewall
isolation from guests or hostile devices. The hook currently supports IPv4 only.
Do not connect it directly to an untrusted network.

The stock entry reuses the already-proven plugin signature-path confusion:
the genuine cached asset-7 image remains the signature anchor while the selected
higher-version image provides the boot indexer hook. This is not a genuine vendor
signature on the modified image. Supported versions/manifest rules are narrow.
It does not alter conditional-access entitlements, bypass content authorization,
decrypt satellite recordings or write signed flash firmware.

This shareable bundle excludes Jellyfin tokens/config/account checkpoints,
YouTube/Google cookies, Wi-Fi/NVRAM state, phone preferences/signing keys,
recordings, disk images and your private session/chat logs. Receiver `/var`
backups contain private information; do not share them. The CA bundle is public.
The IPTV playlist contains no channels/URLs; bring your own legal sources.

The package includes necessary vendor plugin/sysroot components. They are not
declared open source; check redistribution rights before publishing it. Third
party notices included here do not grant rights to vendor binaries. Do not
assume this snapshot is a finished publicly redistributable product.

The developing module manager introduces a separate privileged management API.
**Third-party modules execute receiver-side code and must only be installed from
trusted sources.** Process separation is not a security sandbox. URL installation
and enable/disable/uninstall require a management bearer; see
[the module protocol](MODULES.md#management-authorization-and-install-transaction).
The shipped legacy payload has not yet been switched to this new daemon.
