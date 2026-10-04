#!/usr/bin/env bash
# Never accepts block devices. Physical disk writes are a separate manual step.
set -euo pipefail
bundle=$(cd -- "$(dirname -- "$0")/.." && pwd)
usage() {
    echo 'Usage: prepare-stock-image.sh audit STOCK_VAR_CLONE CONFIG'
    echo '       prepare-stock-image.sh install WORKING_VAR_CLONE CONFIG ORIGINAL_VAR_BACKUP INSTALL_IN_CLONED_VAR_IMAGE'
    exit 2
}
[[ $# -ge 3 ]] || usage
mode=$1
var_image=$(realpath -e -- "$2")
config=$(realpath -e -- "$3")
case "$var_image$config" in *','*|*$'\n'*) echo 'Commas/newlines in VM input paths are not supported'; exit 1;; esac
case "$mode" in audit) [[ $# = 3 ]] || usage;; install) [[ $# = 5 && $5 = INSTALL_IN_CLONED_VAR_IMAGE ]] || usage;; *) usage;; esac
[[ -f "$var_image" && ! -b "$var_image" && ! -L "$2" ]] || { echo 'Only a regular cloned partition image is accepted'; exit 1; }
[[ $(stat -c %s "$var_image") = 16113320448 ]] || { echo 'Unsupported /var size; do not bypass the geometry guard'; exit 1; }
for cmd in qemu-system-mips gzip cpio md5sum sha256sum; do command -v "$cmd" >/dev/null; done
python3 - "$config" <<'PY'
import ipaddress, pathlib, sys
lines = [s for s in pathlib.Path(sys.argv[1]).read_text().splitlines() if s and not s.startswith('#')]
assert len(lines) == 2, 'Expected only JELLYFIN_IPV4 and JELLYFIN_PORT'
values = dict(s.split('=', 1) for s in lines)
assert set(values) == {'JELLYFIN_IPV4', 'JELLYFIN_PORT'}
ip = ipaddress.IPv4Address(values['JELLYFIN_IPV4'])
assert not ip.is_unspecified and not ip.is_multicast and not ip.is_loopback
assert str(ip) != '192.0.2.10', 'Replace the documentation example address'
assert values['JELLYFIN_PORT'].isdigit() and 0 < int(values['JELLYFIN_PORT']) <= 65535
PY
if [[ $mode = install ]]; then
    authority=$(realpath -e -- "$4")
    [[ -f "$authority" && ! -L "$4" && ! "$authority" -ef "$var_image" ]] || { echo 'An independent unchanged partition backup is required'; exit 1; }
    [[ $(stat -c %s "$authority") = 16113320448 ]] || exit 1
    echo 'Checking that the working clone exactly matches the original backup...'
    cmp -- "$authority" "$var_image" || { echo 'Working clone differs; start from a fresh copy'; exit 1; }
fi
# Immutable public files are verified before generating a configured initramfs.
(cd "$bundle"; sha256sum -c SHA256SUMS >/dev/null)
work=$(mktemp -d "${TMPDIR:-/tmp}/rebox-clone.XXXXXX")
# Leave logs/initramfs for inspection; no recursive deletion of user data.
echo "Work directory (retained): $work"
mkdir -p "$work/overlay/modules" "$work/overlay/payload" "$work/modules-source" "$work/overlay/bin" "$work/overlay/lib" "$work/overlay/dev" "$work/overlay/proc" "$work/overlay/sys" "$work/overlay/etc"
stock=$bundle/stock-bootstrap
kver=4.19.0-21-4kc-malta
(cd "$work/overlay"; gzip -dc "$stock/base-initrd.gz" | cpio -idmu --quiet \
    bin/busybox lib/ld-2.28.so lib/ld.so.1 lib/libc-2.28.so lib/libc.so.6)
for applet in ash sh mount umount mkdir insmod sleep cat sha256sum grep cmp md5sum cp mv sync mktemp chmod chown poweroff; do
    ln -s busybox "$work/overlay/bin/$applet"
done
(cd "$work/modules-source"; gzip -dc "$stock/base-initrd.gz" | cpio -idmu --quiet \
    "lib/modules/$kver/kernel/drivers/virtio/virtio.ko" \
    "lib/modules/$kver/kernel/drivers/virtio/virtio_ring.ko" \
    "lib/modules/$kver/kernel/drivers/virtio/virtio_pci.ko" \
    "lib/modules/$kver/kernel/crypto/crc32c_generic.ko" \
    "lib/modules/$kver/kernel/lib/libcrc32c.ko")
cp "$work/modules-source/lib/modules/$kver/kernel/drivers/virtio/"*.ko "$work/overlay/modules/"
cp "$work/modules-source/lib/modules/$kver/kernel/crypto/crc32c_generic.ko" "$work/overlay/modules/"
cp "$work/modules-source/lib/modules/$kver/kernel/lib/libcrc32c.ko" "$stock/virtio_blk.ko" "$stock/xfs.ko" "$work/overlay/modules/"
cp "$stock/guest-init" "$work/overlay/init"
chmod 755 "$work/overlay/init"
cp -a "$bundle/receiver/payload/hr54-persist" "$work/overlay/payload/"
cp "$stock/7_6933_6840.squashfs" "$stock/7_6933_6840.sig" "$work/overlay/payload/"
cp "$config" "$work/overlay/payload/rebox.conf"
# Do not unpack the entire Debian installer plus media runtime into lowmem.
# A minimal BusyBox/libc + exact modules + payload image is sufficient.
(cd "$work/overlay"; find . -print0 | cpio --null -o -H newc --owner=0:0 --quiet) | gzip -1 > "$work/rebox-initrd.gz"
# Sparse placeholder, not the physical recording partition.
truncate -s 983406247936 "$work/rt-placeholder.img"
run_vm() {
    local operation=$1 readonly=$2 log=$3
    timeout 900 qemu-system-mips -M malta -m 512 -nographic -no-reboot \
        -kernel "$stock/vmlinux-4.19.0-21-4kc-malta" -initrd "$work/rebox-initrd.gz" \
        -append "console=ttyS0 panic=-1 rebox_mode=$operation" \
        -drive "file=$var_image,format=raw,if=virtio,cache=none,aio=threads,readonly=$readonly" \
        -drive "file=$work/rt-placeholder.img,format=raw,if=virtio,cache=none,aio=threads" \
        | tee "$log"
}
# Always perform an independent read-only run before any image writes.
run_vm audit on "$work/audit.log"
grep -q '^REBOX_STOCK_READONLY_PASS' "$work/audit.log"
if [[ $mode = audit ]]; then echo 'Stock clone audit passed; no image writes performed'; exit 0; fi
run_vm install off "$work/install.log"
grep -q '^REBOX_CLONE_INSTALL_PASS' "$work/install.log"
run_vm installed on "$work/verify.log"
grep -q '^REBOX_INSTALLED_READONLY_PASS' "$work/verify.log"
echo 'Working /var clone prepared and verified read-only. No physical disk was attached or written by this script.'
echo 'Native activation is intentionally absent. Follow docs/STOCK-INSTALL.md.'
