#!/bin/sh
# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
#
# Turns a finished Unix/Android SpiderMonkey build into the flat artifact
# directory ais2api's AIS2API_SPIDERMONKEY_DIR names:
#
#   include/mozjs.h                    the embedding shim's C interface
#   lib/libmozjs_shim.a                the shim, compiled with -fPIC
#   lib/libjs_static.a                 SpiderMonkey's C++ objects, as a full archive
#   lib/libjsrust.a                    its Rust code
#   licenses/LICENSE                   Mozilla Public License 2.0
#   licenses/ICU-LICENSE.txt           Unicode License v3 (ICU4C / ICU4X)
#   licenses/THIRD_PARTY_NOTICES.txt   bundled third-party notices
#   BUILDINFO.json                     versions, digests and what to link
#
# Usage: package-unix.sh <target> <objdir> <mozbuild state dir> <output dir> [source tarball] [archive.tar.xz]
set -eu

if [ $# -lt 4 ]; then
    echo "usage: package-unix.sh <target> <objdir> <mozbuild state dir> <output dir> [source tarball] [archive.tar.xz]" >&2
    exit 2
fi

target="$1"
objdir=$(cd "$2" && pwd)
state=$(cd "$3" && pwd)
mkdir -p "$(dirname "$4")"
out="$(cd "$(dirname "$4")" && pwd)/$(basename "$4")"
tarball=${5:-}
archive=${6:-}
here=$(cd "$(dirname "$0")" && pwd)

if [ -d "$here/mozjs" ]; then
    shim="$here/mozjs"
else
    shim=$(cd "$here/../../waa/third_party/aistudio2api/mozjs" && pwd)
fi

case "$target" in
    linux-x64)
        rust_target=x86_64-unknown-linux-gnu
        mozconfig_file="$here/mozconfig-linux-x64"
        cxx="$state/clang/bin/clang++"
        llvm_ar="$state/clang/bin/llvm-ar"
        extra_shim_flags=""
        sys_libs='"pthread", "dl", "m"'
        toolchain_extra='"glibc": "'"$(ldd --version 2>/dev/null | head -n 1 | sed 's/.* //')"'"'
        ;;
    linux-arm64)
        rust_target=aarch64-unknown-linux-gnu
        mozconfig_file="$here/mozconfig-linux-arm64"
        cxx="$state/clang/bin/clang++"
        llvm_ar="$state/clang/bin/llvm-ar"
        extra_shim_flags=""
        sys_libs='"pthread", "dl", "m"'
        toolchain_extra='"glibc": "'"$(ldd --version 2>/dev/null | head -n 1 | sed 's/.* //')"'"'
        ;;
    android-arm64)
        if [ -z "${ANDROID_NDK_HOME:-}" ]; then
            echo "ANDROID_NDK_HOME must be set when packaging android-arm64" >&2
            exit 1
        fi
        rust_target=aarch64-linux-android
        mozconfig_file="$here/mozconfig-android-arm64"
        ndk_bin="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
        cxx="$ndk_bin/aarch64-linux-android24-clang++"
        llvm_ar="$ndk_bin/llvm-ar"
        extra_shim_flags="-DANDROID -D__ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__ -fno-short-enums"
        sys_libs='"dl", "m", "log"'
        ndk_rev=$(sed -n 's/^Pkg\.Revision = //p' "$ANDROID_NDK_HOME/source.properties" 2>/dev/null | tr -d '\r' || true)
        toolchain_extra='"androidNdk": "'"$ndk_rev"'", "androidApi": 24'
        ;;
    *)
        echo "unsupported target: $target" >&2
        exit 2
        ;;
esac

for file in "$objdir/js/src/build/libjs_static_a.list" "$objdir/$rust_target/release/libjsrust.a" \
            "$cxx" "$llvm_ar" "$shim/mozjs.cpp" "$shim/mozjs.h" "$mozconfig_file" \
            "$here/licenses/LICENSE" "$here/licenses/ICU-LICENSE.txt" "$here/licenses/THIRD_PARTY_NOTICES.txt"; do
    [ -f "$file" ] || { echo "missing: $file" >&2; exit 1; }
done

version=$(sed -n 's/^Version: //p' "$objdir/js/src/build/js.pc")
[ -n "$version" ] || { echo "cannot read the SpiderMonkey version from js.pc" >&2; exit 1; }

rm -rf "$out"
mkdir -p "$out/include" "$out/lib" "$out/licenses" "$out/obj"

# Repack Mozilla's thin archive object list into a standalone non-thin archive.
(
    cd "$objdir/js/src/build"
    "$llvm_ar" rcs "$out/lib/libjs_static.a" "@$objdir/js/src/build/libjs_static_a.list"
)
cp "$objdir/$rust_target/release/libjsrust.a" "$out/lib/libjsrust.a"

# Compile the pure extern "C" embedding shim with -fPIC and matching ABI flags.
# shellcheck disable=SC2086
"$cxx" -std=gnu++20 -O2 -fPIC -fno-rtti -fno-exceptions -fno-sized-deallocation \
    -ffunction-sections -fdata-sections -DNDEBUG -DSTATIC_JS_API -DJS_STANDALONE \
    -DXP_UNIX -DXP_LINUX $extra_shim_flags \
    -I "$objdir/dist/include" -I "$shim" \
    -c "$shim/mozjs.cpp" -o "$out/obj/mozjs.o"
"$llvm_ar" rcs "$out/lib/libmozjs_shim.a" "$out/obj/mozjs.o"
rm -rf "$out/obj"

for lib in "$out/lib/libmozjs_shim.a" "$out/lib/libjs_static.a" "$out/lib/libjsrust.a"; do
    [ "$(head -c 7 "$lib")" = '!<arch>' ] || { echo "not a non-thin static archive: $lib" >&2; exit 1; }
done

cp "$shim/mozjs.h" "$out/include/mozjs.h"
cp "$here/licenses/LICENSE" "$out/licenses/LICENSE"
cp "$here/licenses/ICU-LICENSE.txt" "$out/licenses/ICU-LICENSE.txt"
cp "$here/licenses/THIRD_PARTY_NOTICES.txt" "$out/licenses/THIRD_PARTY_NOTICES.txt"

digest() { sha256sum "$1" | cut -d' ' -f1; }
clang_version=$("$cxx" --version | head -n 1 | sed -n 's/.*clang version \([0-9.]*\).*/\1/p')
rustc_version=$(sed -n 's/^RUSTC_VERSION = //p' "$objdir/config/autoconf.mk" 2>/dev/null || true)
source_digest=""
[ -n "$tarball" ] && source_digest=$(sha512sum "$tarball" | cut -d' ' -f1)

{
    printf '{\n'
    printf '  "spidermonkey": "%s",\n' "$version"
    printf '  "target": "%s",\n' "$target"
    printf '  "source": {"tarball": "firefox-%s.source.tar.xz", "sha512": "%s"},\n' "$version" "$source_digest"
    printf '  "patches": {\n'
    first=1
    for patch in "$here"/patches/*.patch; do
        [ $first -eq 1 ] || printf ',\n'
        first=0
        printf '    "%s": "%s"' "$(basename "$patch")" "$(digest "$patch")"
    done
    printf '\n  },\n'
    printf '  "mozconfig": "%s",\n' "$(digest "$mozconfig_file")"
    printf '  "shim": {"mozjs.cpp": "%s", "mozjs.h": "%s"},\n' "$(digest "$shim/mozjs.cpp")" "$(digest "$shim/mozjs.h")"
    printf '  "toolchain": {"clang": "%s", "rustc": "%s", %s},\n' \
        "$clang_version" "$rustc_version" "$toolchain_extra"
    printf '  "files": {\n'
    printf '    "include/mozjs.h": "%s",\n' "$(digest "$out/include/mozjs.h")"
    printf '    "lib/libmozjs_shim.a": "%s",\n' "$(digest "$out/lib/libmozjs_shim.a")"
    printf '    "lib/libjs_static.a": "%s",\n' "$(digest "$out/lib/libjs_static.a")"
    printf '    "lib/libjsrust.a": "%s",\n' "$(digest "$out/lib/libjsrust.a")"
    printf '    "licenses/LICENSE": "%s",\n' "$(digest "$out/licenses/LICENSE")"
    printf '    "licenses/ICU-LICENSE.txt": "%s",\n' "$(digest "$out/licenses/ICU-LICENSE.txt")"
    printf '    "licenses/THIRD_PARTY_NOTICES.txt": "%s"\n' "$(digest "$out/licenses/THIRD_PARTY_NOTICES.txt")"
    printf '  },\n'
    printf '  "link": ["lib/libmozjs_shim.a", "lib/libjs_static.a", "lib/libjsrust.a", %s]\n' "$sys_libs"
    printf '}\n'
} > "$out/BUILDINFO.json"
echo "packaged SpiderMonkey $version ($target) in $out"

if [ -n "$archive" ]; then
    mkdir -p "$(dirname "$archive")"
    LC_ALL=C tar -C "$out" --sort=name --owner=0 --group=0 --numeric-owner -cf - BUILDINFO.json include lib licenses | xz -T0 -6 > "$archive"
    echo "archived $archive (sha256=$(sha256sum "$archive" | cut -d' ' -f1))"
fi
