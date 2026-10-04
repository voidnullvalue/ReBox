#!/bin/sh
# Build hr54-doom.
#
#   tools/build-engine.sh              cross-compile for the receiver
#   tools/build-engine.sh --host       build natively, for testing
#
# The receiver is big-endian MIPS32 o32 on uClibc.  The whole vendor stack
# (kernel, busybox, libcwebkit, libdtvwm, and the existing hr54-jf) is
# ELFDATA2MSB, so a little-endian build is useless here.  -mcpu=mips32 is
# deliberate: this CPU is NOT MIPS32r2 and r2 instructions trap.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ENGINE="$HERE/../engine"
ZIG=${ZIG:-/tmp/opencode/zig/zig}
OUT=${OUT:-$ENGINE/../bin/hr54-doom}

CFLAGS="-O2 -std=gnu99 -Wall -DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE
        -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200
        -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable
        -Wno-implicit-fallthrough -Wno-format-truncation -Wno-stringop-truncation
        -I$ENGINE"

# doomgeneric upstream list, minus the platform files this build does not use.
SRC="doomgeneric_hr54 am_map doomdef doomstat dstrings d_event d_items d_iwad
     d_loop d_main d_mode d_net f_finale f_wipe g_game hu_lib hu_stuff info
     i_cdmus i_endoom i_joystick i_scale i_sound i_system i_timer memio m_argv
     m_bbox m_cheat m_config m_controls m_fixed m_menu m_misc m_random p_ceilng
     p_doors p_enemy p_floor p_inter p_lights p_map p_maputl p_mobj p_plats
     p_pspr p_saveg p_setup p_sight p_spec p_switch p_telept p_tick
     p_user r_bsp r_data r_draw r_main r_plane r_segs r_sky r_things sha1 sounds
     statdump st_lib st_stuff s_sound tables v_video wi_stuff w_checksum
     w_file w_main w_wad z_zone w_file_stdc i_input i_video doomgeneric"

if [ "${1:-}" = "--native" ]; then
    project=$(cd "$HERE/../.." && pwd)
    ui="$project/tools/native-ui"
    native_zig=${HR54_ZIG:-/tmp/hr54-zig/zig}
    native_sysroot=${HR54_SYSROOT:-$project/extracted/sdb4-rootfs}
    mkdir -p "$(dirname "$OUT")"
    # Same receiver uClibc startup/link as native-ui/build-receiver.sh.
    # Freestanding integer and local libc declarations, never musl structs.
    "$native_zig" cc -target mips-linux-musleabi -mcpu=mips32 -msoft-float \
        $CFLAGS -Wno-unknown-warning-option -ffreestanding -fno-stack-protector -fno-builtin-fprintf \
        -nostdinc -isystem "$(dirname "$native_zig")/lib/include" \
        -I"$ui/receiver-include" -I"$ui" -DHR54_RECEIVER -DHR54_NATIVE_VIDEO \
        -nostdlib -Wl,-s -Wl,-e,__start -Wl,--dynamic-linker=/lib/ld-uClibc.so.0 \
        -Wl,--allow-shlib-undefined -Wl,-rpath,/opt/opengl/lib:/opt/sys_monitor/lib \
        -o "$OUT" $(for s in $SRC; do echo "$ENGINE/$s.c"; done) \
        "$ENGINE/doom_video_native.c" "$ENGINE/doom_input_native.c" \
        "$ui/native_egl.c" "$ui/framebuffer.c" "$ui/receiver_bindings.c" "$ui/receiver_start.c" \
        "$native_sysroot/opt/opengl/lib/libopengl.so.1" \
        "$native_sysroot/lib/libc.so.0" "$native_sysroot/lib/librt.so.0" \
        "$native_sysroot/lib/libm.so.0"
    readelf -h -l -d "$OUT"
elif [ "${1:-}" = "--host" ]; then
    CC=${CC:-cc}
    $CC $CFLAGS -o "$OUT" $(for s in $SRC; do echo "$ENGINE/$s.c"; done) -lm
else
    # mips-linux-musleabi is *big-endian* musl; mipsel would be wrong.
    $ZIG cc -target mips-linux-musleabi -mcpu=mips32 -static $CFLAGS \
        -o "$OUT" $(for s in $SRC; do echo "$ENGINE/$s.c"; done) -lm
fi
echo "built $OUT"
file "$OUT" 2>/dev/null || true
ls -l "$OUT"
