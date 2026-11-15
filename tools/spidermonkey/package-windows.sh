#!/bin/sh
# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
#
# Turns a finished SpiderMonkey build (mozconfig-windows) into the artifact
# directory ais2api's AIS2API_SPIDERMONKEY_DIR names:
#
#   include/mozjs.h                    the embedding shim's C interface
#   lib/mozjs_shim.lib                 the shim, compiled with the build's clang-cl
#   lib/js_static.lib                  SpiderMonkey's C++ objects, as a full archive
#   lib/jsrust.lib                     its Rust code
#   licenses/LICENSE                   Mozilla Public License 2.0
#   licenses/ICU-LICENSE.txt           Unicode License v3 (ICU4C / ICU4X)
#   licenses/THIRD_PARTY_NOTICES.txt   bundled third-party notices
#   BUILDINFO.json                     versions, digests and what to link
#
# Usage: package-windows.sh <objdir> <mozbuild state dir> <output dir> [source tarball] [archive.tar.xz]
#
# The build's js_static.lib is a thin archive that only names its objects;
# MSVC's linker cannot read one, so its object list is archived again here.
set -eu

# Native tools are given Windows paths ("D:/sm/..."), which MSYS shells
# would not convert inside joined options such as -Fo<path>.
native() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else printf '%s\n' "$1"; fi; }
posix() { if command -v cygpath >/dev/null 2>&1; then cygpath -u "$1"; else printf '%s\n' "$1"; fi; }
objdir=$(native "$1")
state=$(native "$2")
out=$(native "$3")
out_posix=$(posix "$3")
tarball=${4:-}
archive=${5:-}
here_posix=$(cd "$(dirname "$0")" && pwd)
here=$(native "$here_posix")
if [ -d "$here_posix/mozjs" ]; then
    shim=$(native "$here_posix/mozjs")
else
    shim=$(native "$(cd "$here_posix/../../waa/third_party/aistudio2api/mozjs" && pwd)")
fi
# Already converted: keep MSYS from rewriting them again.
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'
clang="$state/clang/bin"
sysroot="$state/vs"

for file in "$objdir/js/src/build/js_static_lib.list" "$objdir/x86_64-pc-windows-msvc/release/jsrust.lib" \
            "$clang/clang-cl.exe" "$clang/llvm-lib.exe" "$shim/mozjs.cpp" "$shim/mozjs.h" \
            "$here/licenses/LICENSE" "$here/licenses/ICU-LICENSE.txt" "$here/licenses/THIRD_PARTY_NOTICES.txt"; do
    [ -f "$file" ] || { echo "missing: $file" >&2; exit 1; }
done

version=$(sed -n 's/^Version: //p' "$objdir/js/src/build/js.pc")
[ -n "$version" ] || { echo "cannot read the SpiderMonkey version from js.pc" >&2; exit 1; }

rm -rf "$out"
mkdir -p "$out/include" "$out/lib" "$out/licenses" "$out/obj"

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

for lib in "$out/lib/mozjs_shim.lib" "$out/lib/js_static.lib" "$out/lib/jsrust.lib"; do
    [ "$(head -c 7 "$lib")" = '!<arch>' ] || { echo "not a non-thin static archive: $lib" >&2; exit 1; }
done

cp "$shim/mozjs.h" "$out/include/mozjs.h"
cp "$here/licenses/LICENSE" "$out/licenses/LICENSE"
cp "$here/licenses/ICU-LICENSE.txt" "$out/licenses/ICU-LICENSE.txt"
cp "$here/licenses/THIRD_PARTY_NOTICES.txt" "$out/licenses/THIRD_PARTY_NOTICES.txt"

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
    printf '    "lib/jsrust.lib": "%s",\n' "$(digest "$out/lib/jsrust.lib")"
    printf '    "licenses/LICENSE": "%s",\n' "$(digest "$out/licenses/LICENSE")"
    printf '    "licenses/ICU-LICENSE.txt": "%s",\n' "$(digest "$out/licenses/ICU-LICENSE.txt")"
    printf '    "licenses/THIRD_PARTY_NOTICES.txt": "%s"\n' "$(digest "$out/licenses/THIRD_PARTY_NOTICES.txt")"
    printf '  },\n'
    printf '  "link": ["lib/mozjs_shim.lib", "lib/js_static.lib", "lib/jsrust.lib",\n'
    printf '           "advapi32.lib", "bcrypt.lib", "dbghelp.lib", "delayimp.lib", "mincore.lib", "ntdll.lib",\n'
    printf '           "ole32.lib", "oleaut32.lib", "shell32.lib", "synchronization.lib", "user32.lib", "userenv.lib",\n'
    printf '           "uuid.lib", "winmm.lib", "ws2_32.lib"]\n'
    printf '}\n'
} > "$out/BUILDINFO.json"
echo "packaged SpiderMonkey $version (windows-x64) in $out"

if [ -n "$archive" ]; then
    archive_posix=$(posix "$archive")
    mkdir -p "$(dirname "$archive_posix")"
    unset MSYS_NO_PATHCONV MSYS2_ARG_CONV_EXCL
    LC_ALL=C tar -C "$out_posix" -cf - BUILDINFO.json include lib licenses | xz -T0 -6 > "$archive_posix"
    echo "archived $archive_posix (sha256=$(sha256sum "$archive_posix" | cut -d' ' -f1))"
fi
