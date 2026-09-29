#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SDK_ROOT="${SDK_ROOT:-/home/kinho/Lyra-sdk}"
BR_OUTPUT="${BUILDROOT_OUTPUT:-$SDK_ROOT/buildroot/output/rockchip_rk3506_luckfox}"
HOST_DIR="${BUILDROOT_HOST_DIR:-$BR_OUTPUT/host}"
SYSROOT="$HOST_DIR/arm-buildroot-linux-gnueabihf/sysroot"
BUILD_ROOT="${PIPEMIXER_BUILD_ROOT:-$PROJECT_DIR/.cache/build-rk3506}"
SOURCE_DIR="$BUILD_ROOT/source"
MESON_BUILD_DIR="$BUILD_ROOT/build"
CROSS_FILE="$BUILD_ROOT/meson-cross.ini"
JOBS="${JOBS:-2}"

if [[ ! "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
    echo "JOBS must be a positive integer" >&2
    exit 2
fi

for tool in meson ninja pkgconf arm-buildroot-linux-gnueabihf-gcc \
            arm-buildroot-linux-gnueabihf-g++ arm-buildroot-linux-gnueabihf-gcc-ar \
            arm-buildroot-linux-gnueabihf-strip; do
    if [[ ! -x "$HOST_DIR/bin/$tool" ]]; then
        echo "Missing SDK tool: $HOST_DIR/bin/$tool" >&2
        echo "Build the rockchip_rk3506_luckfox Buildroot host tools first." >&2
        exit 1
    fi
done

if [[ ! -d "$SYSROOT/usr/include" ]]; then
    echo "Missing Buildroot target sysroot: $SYSROOT" >&2
    exit 1
fi

export PATH="$HOST_DIR/bin:$PATH"
export PKG_CONFIG="$HOST_DIR/bin/pkgconf"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig"

if ! "$HOST_DIR/bin/pkgconf" --exists libpipewire-0.3 ncursesw inih; then
    echo "PipeMixer dependencies are missing from the SDK sysroot." >&2
    echo "Required staged modules: libpipewire-0.3, ncursesw, inih." >&2
    exit 1
fi

command -v rsync >/dev/null || { echo "rsync is required" >&2; exit 1; }
mkdir -p "$SOURCE_DIR" "$MESON_BUILD_DIR"

# Keep the Meson build tree and GCC 12 compatibility normalization in this
# project-local snapshot, separate from the checked-out source files.
rsync -a --delete --exclude='/.git/' --exclude='/.cache/' \
    "$PROJECT_DIR/" "$SOURCE_DIR/"
sed -i 's/uint64_t){}/uint64_t){0}/g' "$SOURCE_DIR/src/events.c"
sed -i 's/pw_id_t){}/pw_id_t){0}/g' \
    "$SOURCE_DIR/src/pw/node.c" "$SOURCE_DIR/src/pw/device.c"

cat > "$CROSS_FILE" <<EOF
[binaries]
c = '$HOST_DIR/bin/arm-buildroot-linux-gnueabihf-gcc'
cpp = '$HOST_DIR/bin/arm-buildroot-linux-gnueabihf-g++'
ar = '$HOST_DIR/bin/arm-buildroot-linux-gnueabihf-gcc-ar'
strip = '$HOST_DIR/bin/arm-buildroot-linux-gnueabihf-strip'
cmake = '$HOST_DIR/bin/cmake'
pkg-config = '$HOST_DIR/bin/pkgconf'

[built-in options]
c_args = ['-D_LARGEFILE_SOURCE', '-D_LARGEFILE64_SOURCE', '-D_FILE_OFFSET_BITS=64', '-D_TIME_BITS=64', '-O2', '-g0', '-D_FORTIFY_SOURCE=1']
c_link_args = []
cpp_args = ['-D_LARGEFILE_SOURCE', '-D_LARGEFILE64_SOURCE', '-D_FILE_OFFSET_BITS=64', '-D_TIME_BITS=64', '-O2', '-g0', '-D_FORTIFY_SOURCE=1']
cpp_link_args = []
wrap_mode = 'nodownload'
cmake_prefix_path = '$SYSROOT/usr/lib/cmake'

[properties]
needs_exe_wrapper = true
sys_root = '$SYSROOT'
pkg_config_libdir = '$PKG_CONFIG_LIBDIR'
pkg_config_static = 'false'
cmake_toolchain_file = '$HOST_DIR/share/buildroot/toolchainfile.cmake'
cmake_defaults = false

[host_machine]
system = 'linux'
cpu_family = 'arm'
cpu = 'cortex-a7'
endian = 'little'
EOF

MESON_ARGS=(
    --cross-file "$CROSS_FILE"
    --prefix=/usr
    --libdir=lib
    --default-library=shared
    --buildtype=release
    -Dstrip=false
    -Dshell-completions=false
)

if [[ -f "$MESON_BUILD_DIR/meson-private/coredata.dat" ]]; then
    "$HOST_DIR/bin/meson" setup --reconfigure "$MESON_BUILD_DIR" "${MESON_ARGS[@]}"
else
    "$HOST_DIR/bin/meson" setup "$MESON_BUILD_DIR" "$SOURCE_DIR" "${MESON_ARGS[@]}"
fi

"$HOST_DIR/bin/ninja" -C "$MESON_BUILD_DIR" -j "$JOBS"

BINARY="$MESON_BUILD_DIR/pipemixer"
if [[ ! -x "$BINARY" ]]; then
    echo "Build finished without producing $BINARY" >&2
    exit 1
fi

echo "PipeMixer built: $BINARY"
echo "Run with: $BINARY"
