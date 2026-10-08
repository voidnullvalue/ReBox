# Android controller

APK: `android/ReBox-controller.apk`.
SHA-256: `d41c7684fecafd696fb07acb8e6122cf2719aec59a2535fe35209b2494d3cf81`.
Package: `com.hr54.controller`, version 1.0.0/code 1, debug-signed.
Minimum Android 8/API 26; native voice library is **arm64-v8a only**.

Copy/open the APK on the phone and permit installation from your chosen file
manager, or install through an authorized USB-debugging session:

```sh
adb devices
adb -s YOUR_PHONE_SERIAL install -r android/ReBox-controller.apk
```

Set the receiver URL to `http://YOUR_RECEIVER_IPV4:8130`, not the Jellyfin server.
The phone/receiver must be on the same trusted network. Jellyfin authentication
is owned by the receiver. Grant microphone permission only if using voice input.
The bundled voice model accounts for most of the approximately 72 MiB APK.

If install reports `INSTALL_FAILED_UPDATE_INCOMPATIBLE`, the existing app has a
different signing certificate. Do not uninstall silently: app-private state is
lost. Have the owner approve removal/reconfigure, or use their original key to
build an update. Fresh install commands, only after that decision:

```sh
adb -s YOUR_PHONE_SERIAL uninstall com.hr54.controller
adb -s YOUR_PHONE_SERIAL install android/ReBox-controller.apk
```

No keystore/private key or phone data is shipped. Source rebuilds will use YOUR
debug/signing identity, which may not match this APK for future in-place updates.
Jellyfin play requests use `returnToTv=false`; native mode never requests stock
satellite/setup presentation. API reproduction and an actual phone-originated
request are separate acceptance checks.

The radio/surfing update rebuilds the controller with capability-driven CH+/CH−
controls. All 55 host tests pass. The packaged APK retains the preceding APK’s
signing certificate. Its current SHA-256 is `0b4ffa3d05659ea684832a147d1953773b5e22f5b2125802c6ebb2d79a2cbfb8`.
