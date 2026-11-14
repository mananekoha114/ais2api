#!/bin/sh
# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
#
# Turns a finished SpiderMonkey build (mozconfig-windows) into the artifact
# directory ais2api's AIS2API_SPIDERMONKEY_DIR names:
#
#   include/mozjs.h          the embedding shim's C interface
#   lib/mozjs_shim.lib       the shim, compiled with the build's clang-cl
#   lib/js_static.lib        SpiderMonkey's C++ objects, as a full archive
#   lib/jsrust.lib           its Rust code
#   BUILDINFO.json           versions, digests and what to link
#
# Usage: package-windows.sh <objdir> <mozbuild state dir> <output dir> [source tarball]
#
# The build's js_static.lib is a thin archive that only names its objects;
# MSVC's linker cannot read one, so its object list is archived again here.
set -eu

# Native tools are given Windows paths ("E:/sm/..."), which MSYS shells
# would not convert inside joined options such as -Fo<path>.
native() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else printf '%s
' "$1"; fi; }
objdir=$(native "$1")
state=$(native "$2")
out=$(native "$3")
tarball=${4:-}
here=$(native "$(cd "$(dirname "$0")" && pwd)")
shim=$(native "$(cd "$here/../../waa/third_party/aistudio2api/mozjs" && pwd)")
# Already converted: keep MSYS from rewriting them again.
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'
clang="$state/clang/bin"
sysroot="$state/vs"

for file in "$objdir/js/src/build/js_static_lib.list" "$objdir/x86_64-pc-windows-msvc/release/jsrust.lib" \
            "$clang/clang-cl.exe" "$clang/llvm-lib.exe" "$shim/mozjs.cpp" "$shim/mozjs.h"; do
    [ -f "$file" ] || { echo "missing: $file" >&2; exit 1; }
done

version=$(sed -n 's/^Version: //p' "$objdir/js/src/build/js.pc")
[ -n "$version" ] || { echo "cannot read the SpiderMonkey version from js.pc" >&2; exit 1; }

rm -rf "$out"
mkdir -p "$out/include" "$out/lib" "$out/obj"

"$clang/llvm-lib.exe" -nologo -out:"$out/lib/js_static.lib" "@$objdir/js/src/build/js_static_lib.list"
cp "$objdir/x86_64-pc-windows-msvc/release/jsrust.lib" "$out/lib/jsrust.lib"

# The shim's flags follow the build's own (config/autoconf.mk) and
# AIStudio2API's cgo flags: C++20, no RTTI, no exceptions, a static engine,
# and the static C runtime.
"$clang/clang-cl.exe" -nologo -fms-compatibility-version=19.50 -std:c++20 -O2 -MT -GR- -D_HAS_EXCEPTIONS=0 \
    -Zc:sizedDealloc- -Gy -Zc:inline -Gw -DNDEBUG -DSTATIC_JS_API -DJS_STANDALONE -DXP_WIN \
    -winsysroot "$sysroot" -imsvc "$objdir/dist/include" -I "$shim" \
    -c "$shim/mozjs.cpp" -Fo"$out/obj/mozjs.obj"
"$clang/llvm-lib.exe" -nologo -out:"$out/lib/mozjs_shim.lib" "$out/obj/mozjs.obj"
rm -rf "$out/obj"
cp "$shim/mozjs.h" "$out/include/mozjs.h"

digest() { sha256sum "$1" | cut -d' ' -f1; }
clang_version=$("$clang/clang-cl.exe" --version | sed -n 's/^clang version \([0-9.]*\).*/\1/p')
rustc_version=$(sed -n 's/^RUSTC_VERSION = //p' "$objdir/config/autoconf.mk" 2>/dev/null || true)
msvc=$(ls "$sysroot/VC/Tools/MSVC" | head -n 1)
sdk=$(ls "$sysroot/Windows Kits/10/Lib" | head -n 1)
source_digest=""
[ -n "$tarball" ] && source_digest=$(sha512sum "$tarball" | cut -d' ' -f1)

# The system libraries are the js shell's (js/src/shell/backend.mk).
{
    printf '{\n'
    printf '  "spidermonkey": "%s",\n' "$version"
    printf '  "target": "windows-x64",\n'
    printf '  "source": {"tarball": "firefox-%s.source.tar.xz", "sha512": "%s"},\n' "$version" "$source_digest"
    printf '  "patches": {\n'
    first=1
    for patch in "$here"/patches/*.patch; do
        [ $first -eq 1 ] || printf ',\n'
        first=0
        printf '    "%s": "%s"' "$(basename "$patch")" "$(digest "$patch")"
    done
    printf '\n  },\n'
    printf '  "mozconfig": "%s",\n' "$(digest "$here/mozconfig-windows")"
    printf '  "shim": {"mozjs.cpp": "%s", "mozjs.h": "%s"},\n' "$(digest "$shim/mozjs.cpp")" "$(digest "$shim/mozjs.h")"
    printf '  "toolchain": {"clang": "%s", "rustc": "%s", "msvc": "%s", "windowsSdk": "%s", "runtime": "MT"},\n' \
        "$clang_version" "$rustc_version" "$msvc" "$sdk"
    printf '  "files": {\n'
    printf '    "include/mozjs.h": "%s",\n' "$(digest "$out/include/mozjs.h")"
    printf '    "lib/mozjs_shim.lib": "%s",\n' "$(digest "$out/lib/mozjs_shim.lib")"
    printf '    "lib/js_static.lib": "%s",\n' "$(digest "$out/lib/js_static.lib")"
    printf '    "lib/jsrust.lib": "%s"\n' "$(digest "$out/lib/jsrust.lib")"
    printf '  },\n'
    printf '  "link": ["lib/mozjs_shim.lib", "lib/js_static.lib", "lib/jsrust.lib",\n'
    printf '           "advapi32.lib", "bcrypt.lib", "dbghelp.lib", "delayimp.lib", "mincore.lib", "ntdll.lib",\n'
    printf '           "ole32.lib", "oleaut32.lib", "shell32.lib", "synchronization.lib", "user32.lib", "userenv.lib",\n'
    printf '           "uuid.lib", "winmm.lib", "ws2_32.lib"]\n'
    printf '}\n'
} > "$out/BUILDINFO.json"
echo "packaged SpiderMonkey $version in $out"
