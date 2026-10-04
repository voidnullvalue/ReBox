# Install onto a stock HR54-700

## 0. Scope and compatibility — do not skip

Use hardware you own or are authorized to modify. This is userland repurposing,
not subscriber-service entitlement, conditional-access or recording decryption.
You need a Linux workstation, Ethernet/trusted LAN, HDMI display, paired remote,
HDD access/adapter, adequate backup storage, and your own Jellyfin server.

The original tested layout is a 1 TB HR54-700 with middleware stack **6840**,
asset-7 manifest minimum **6839**, and genuine cached `7_6932_6932.squashfs`:
SHA-256 `fd0c08e0d6c79a1eb92713639dd720b5b01c743933c1b445351ba4ab0647b75e`.
It is an older specific build, NOT a claim that every stock HR54-700 matches.
Check System Information before disassembly. Cached plugin versions alone are
not sufficient to establish active middleware compatibility.

Exact `/var` partition size: **16,113,320,448 bytes** (31,471,329 sectors).
Exact realtime partition size: **983,406,247,936 bytes** (1,920,715,328 sectors).
The helper refuses other geometries. Do not change guards to force a larger,
smaller or updated receiver through this installer. Native activation additionally
requires all hashes in `receiver/payload/hr54-persist/stock-firmware.md5`,
including stock CAR MD5 `882124071cfe3ac1740af18161995dcf`.

Install workstation commands through your normal distro package manager:
`qemu-system-mips`, Bash, Python 3, GNU coreutils, cpio, gzip, netcat with `-q`,
curl, tar, squashfs-tools. No proprietary cross compiler is needed to install
prebuilt payload. Do NOT run the old `source/hr54-re/hr54-ui/tools/install-receiver.sh`:
it assumes this project's previously installed test system and private addresses.

## 1. Back up and clone — stock has no port 5777 yet

Power off and disconnect the receiver before removing its HDD. Do not hot-remove.
Identify the disk by your own serial, capacity and complete partition geometry
using `lsblk -b -o NAME,PATH,SIZE,START,MODEL,SERIAL,PARTN,MOUNTPOINTS`.
Never assume `/dev/sdb` or reuse the original project's disk serial.
Disable automount. Ensure **every partition is unmounted**. Set the identified
disk read-only with `sudo blockdev --setro /dev/disk/by-id/YOUR_EXACT_DISK`.
Check `blockdev --getro` and save the identification output and partition table.
Back up the entire disk when possible, and at minimum partition 2 plus its exact
geometry. Preserve an unchanged authoritative backup outside the working copy.

Example commands below contain placeholders; resolve them deliberately.

```sh
sudo dd if=/dev/disk/by-id/YOUR_EXACT_DISK-part2 of=stock-var.backup.img bs=4M oflag=excl status=progress
sudo chown "$(id -u):$(id -g)" stock-var.backup.img
sha256sum stock-var.backup.img > stock-var.backup.img.sha256
chmod 400 stock-var.backup.img
cp --reflink=auto --sparse=always stock-var.backup.img rebox-var.working.img
chmod 600 rebox-var.working.img
```

Original partition 2 contains receiver-private metadata/configuration. Keep the
backup private; it is NOT part of the shareable ReBox bundle. Never mount this
filesystem read-write with the workstation's native little-endian XFS driver.
The existing filesystem has endian-sensitive metadata; the proven write route
uses a **big-endian MIPS Linux VM**. No mkfs, repair, partition resizing, flash
operation or physical recording/realtime partition access is part of this setup.
Use a new working filename; never overwrite an existing backup or prepared image.

## 2. Configure and prepare only the cloned image

In ReBox, first run `sha256sum -c SHA256SUMS`. Then:

```sh
cp stock-bootstrap/rebox.conf.example rebox.conf
# Edit rebox.conf: YOUR Jellyfin numeric IPv4 and HTTP port (usually 8096).
tools/prepare-stock-image.sh audit /absolute/path/stock-var.backup.img /absolute/path/ReBox/rebox.conf
tools/prepare-stock-image.sh install /absolute/path/rebox-var.working.img /absolute/path/ReBox/rebox.conf /absolute/path/stock-var.backup.img INSTALL_IN_CLONED_VAR_IMAGE
```

Use absolute paths without commas. The installer verifies bundle hashes,
configuration, exact geometry, untouched stock registry and genuine anchor;
requires the working image to match the original backup; independently audits
read-only; writes only the cloned `/var`; then verifies the installed files in
a fresh read-only VM. It retains its work directory/logs. A sparse dummy realtime
device is generated, not the physical recording partition. Success must include
`REBOX_CLONE_INSTALL_PASS` AND `REBOX_INSTALLED_READONLY_PASS`.
Never restore an image from a failed/partial install. Start with a fresh working
copy of the original backup if any check fails.

This stage adds `/var/hr54-persist` and the two higher-version asset-7 files.
It preserves the genuine anchor, stock registry and existing recordings.
Native presentation is intentionally **not activated** yet.

## 3. Restore the prepared clone to that SAME disk's partition 2

This is the only physical disk write and it overwrites partition 2 metadata.
Recheck disk serial, all geometry, unmounted state, original backup hash and
prepared image size. Only your original, powered-down receiver disk is a target.
Do not write an image made from a different receiver onto this receiver.

```sh
# Replace BOTH placeholders with the exact same identified disk as step 1.
# Confirm this byte-size check AND the saved serial/all partition geometry.
test "$(sudo blockdev --getsize64 /dev/disk/by-id/YOUR_EXACT_DISK-part2)" = 16113320448
test "$(stat -c %s rebox-var.working.img)" = 16113320448
# Also verify no partition of this disk is mounted before making it writable.
sudo blockdev --setrw /dev/disk/by-id/YOUR_EXACT_DISK
sudo dd if=rebox-var.working.img of=/dev/disk/by-id/YOUR_EXACT_DISK-part2 bs=4M conv=fsync status=progress
sudo cmp rebox-var.working.img /dev/disk/by-id/YOUR_EXACT_DISK-part2
sudo blockdev --setro /dev/disk/by-id/YOUR_EXACT_DISK
```

If copy/verification fails, keep the box off and restore the original partition
backup after resolving the failure. Never guess targets. Do not write partitions
1/3/4, whole-disk raw sectors, the partition table, MTD/flash or NVRAM.
Reinstall the HDD while the receiver is off.

## 4. First stock boot: establish root/API access

Use an isolated trusted private LAN. Boot normally and allow 6–8 minutes; the
vendor boot/satellite screen can appear during startup. Find the new DHCP IPv4
address from your router. This is NOT necessarily `192.168.88.103`.

```sh
export HR54_HOST=YOUR_RECEIVER_IPV4
HR54_DRAIN=3 tools/recv/hr54-shell.sh 'id; cat /var/hr54-beachhead/status; cat /proc/sys/kernel/random/boot_id'
curl --max-time 10 "http://$HR54_HOST:8130/api/system/status"
curl --max-time 10 "http://$HR54_HOST:8130/api/state"
```

Expect root, LAN services started, API idle; frontend may say `legacy` until
activation. Stock TV presentation is still enabled, and legacy takeover is
disabled by `--no-launcher`. Do not attempt media playback yet.
If no shell appears, inspect the prepared disk through the read-only VM; do NOT
try random firmware flashing or a factory reset. Restore the original backup
if the hook/firmware is incompatible.

## 5. Guarded native activation

Stop recording/playback and make sure API state is idle. Run:

```sh
HR54_DRAIN=5 tools/recv/hr54-shell.sh '/var/hr54-persist/activate-native.sh ACTIVATE_NATIVE_ON_SUPPORTED_STOCK_HR54'
HR54_DRAIN=5 tools/recv/hr54-shell.sh 'tail -n 35 /var/hr54-persist/native-menu/bootstrap.log; cat /var/hr54-persist/native-menu/rebox-rollback-path'
curl --max-time 10 "http://$HR54_HOST:8130/api/system/status"
```

Activation verifies the entire payload and exact running stock firmware, saves
a new stock CAR/registry/config backup, sets native/broker/suppression markers,
and intentionally restarts only the owned API. Its guarded bootstrap performs
one coordinated stock middleware reload, configures Druid Auto-Start false,
binds the validated CAR policy, starts the vendor RF radio, broker, then UI.
Do not kill siege/Druid individually or perform extra middleware reloads.
Root-shell helper exit status alone does not prove remote script success:
read output, logs, API and the saved rollback path.

## 6. Configure accounts, install phone, physically accept, then reboot

Use native Jellyfin Quick Connect with YOUR account/server. The bundle contains
no login. Install the APK and enter `http://YOUR_RECEIVER_IPV4:8130` in the app;
see [ANDROID.md](ANDROID.md). Replace the intentionally empty
`/var/hr54-persist/jellyfin/iptv/eng.m3u` with your own lawful playlist if wanted.
Frigate's packaged binary currently targets `192.168.88.39:5000`; see build notes
for changing that compile-time target. Other missing sources must not prevent
Home or Jellyfin from working.

Complete [ACCEPTANCE.md](ACCEPTANCE.md) with a REAL physical remote and HDMI
observation. Registration ACKs are not acceptance. Only then reboot normally
and verify that Home, input and playback work again. Retain all backups.
