#!/bin/sh
# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
#
# Builds SpiderMonkey for ais2api on windows-x64 inside the MozillaBuild
# shell; build-windows.cmd sets up that shell. Everything goes below the work
# directory:
#
#   downloads/   the Firefox source tarball and the Rust installer
#   src/         the patched source
#   obj-win64-<version>/   the build
#   dist/windows-x64/   the artifact (package-windows.sh)
#
# Usage: build-windows.sh <work directory>
set -eu

version=158.0.1
rust=1.95.0  # the Rust Firefox 158 is built with (win64-rust-1.95 in taskcluster/kinds/toolchain/rust.yml)
work=$(cygpath -u "$1")
here=$(cd "$(dirname "$0")" && pwd)
downloads="$work/downloads"
source="$work/src/firefox-$version"
objdir="$work/obj-win64-$version"
mkdir -p "$downloads" "$work/src"

# The source tarball, checked against the release's SHA512SUMS.
tarball="$downloads/firefox-$version.source.tar.xz"
release="https://archive.mozilla.org/pub/firefox/releases/$version"
curl -fsSL -o "$downloads/SHA512SUMS" "$release/SHA512SUMS"
expected=$(sed -n "s|^\([0-9a-f]\{128\}\)  source/firefox-$version.source.tar.xz\$|\1|p" "$downloads/SHA512SUMS")
[ -n "$expected" ] || { echo "SHA512SUMS names no source tarball" >&2; exit 1; }
if [ ! -f "$tarball" ] || [ "$(sha512sum "$tarball" | cut -d' ' -f1)" != "$expected" ]; then
    curl -fL -o "$tarball.part" "$release/source/firefox-$version.source.tar.xz"
    [ "$(sha512sum "$tarball.part" | cut -d' ' -f1)" = "$expected" ] || { echo "the source tarball does not match SHA512SUMS" >&2; exit 1; }
    mv "$tarball.part" "$tarball"
fi

# Rust in CARGO_HOME and RUSTUP_HOME, which build-windows.cmd points below the
# work directory.
cargo_bin="$(cygpath -u "$CARGO_HOME")/bin"
if [ ! -x "$cargo_bin/rustup.exe" ]; then
    installer="$downloads/rustup/rustup-init.exe"
    mkdir -p "$downloads/rustup"
    base=https://static.rust-lang.org/rustup/dist/x86_64-pc-windows-msvc
    curl -fsSL -o "$installer" "$base/rustup-init.exe"
    curl -fsSL -o "$installer.sha256" "$base/rustup-init.exe.sha256"
    [ "$(sha256sum "$installer" | cut -d' ' -f1)" = "$(cut -d' ' -f1 < "$installer.sha256")" ] || { echo "rustup-init.exe does not match its checksum" >&2; exit 1; }
    # The check for Visual Studio is pointless here: the build brings its own.
    RUSTUP_INIT_SKIP_MSVC_CHECK=1 "$installer" -y --no-modify-path --profile minimal \
        --default-toolchain "$rust" --default-host x86_64-pc-windows-msvc
fi
# A work directory set up for an earlier Firefox has an older Rust: install
# this version's beside it (nothing to do when it is there) and make the
# rustup proxies that mach runs use it.
"$cargo_bin/rustup.exe" toolchain install "$rust" --profile minimal --no-self-update
export RUSTUP_TOOLCHAIN="$rust"

# The source, patched once.
if [ ! -f "$source/.ais2api-patched" ]; then
    rm -rf "$source"
    # tar -J forks xz itself, which fails at random under MSYS2 (a DLL loads at
    # a different address in the child); the shell starts both instead.
    xz -dc "$tarball" | tar -xf - -C "$work/src"
    [ -f "$source/mach" ] || { echo "extracting the source tarball failed" >&2; exit 1; }
    for patch in "$here"/patches/*.patch; do
        patch -d "$source" -p1 --forward < "$patch"
    done
    touch "$source/.ais2api-patched"
fi

{
    printf '. "%s/mozconfig-windows"\n' "$(cygpath -m "$here")"
    printf 'mk_add_options MOZ_OBJDIR=%s\n' "$(cygpath -m "$objdir")"
} > "$work/mozconfig"
export MOZCONFIG="$(cygpath -m "$work/mozconfig")"
cd "$source"
./mach configure < /dev/null
./mach build < /dev/null

sh "$here/package-windows.sh" "$objdir" "$(cygpath -u "$MOZBUILD_STATE_PATH")" "$work/dist/windows-x64" "$tarball"
