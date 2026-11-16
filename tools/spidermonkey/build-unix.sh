#!/bin/sh
# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
#
# Builds SpiderMonkey 158.0.1 as a static PIC library for ais2api on
# linux-x64, linux-arm64, or android-arm64 (Android NDK r27 LTS API 24).
# Everything goes below the work directory:
#
#   downloads/               Firefox source tarball and rustup-init
#   mozbuild/                bootstrapped toolchains (Clang, cbindgen)
#   cargo/, rustup/          isolated Rust 1.95.0 toolchain
#   src/firefox-<version>/   patched Firefox source
#   obj-<target>-<version>/  SpiderMonkey object directory
#   dist/<target>/           packaged prebuilt directory (package-unix.sh)
#
# Usage: build-unix.sh <linux-x64|linux-arm64|android-arm64> <work directory> [archive.tar.xz]
set -eu

if [ $# -lt 2 ]; then
    echo "usage: build-unix.sh <linux-x64|linux-arm64|android-arm64> <work directory> [archive.tar.xz]" >&2
    exit 2
fi

target="$1"
mkdir -p "$2"
work=$(cd "$2" && pwd)
archive=${3:-}
if [ -n "$archive" ]; then
    mkdir -p "$(dirname "$archive")"
    archive="$(cd "$(dirname "$archive")" && pwd)/$(basename "$archive")"
fi
here=$(cd "$(dirname "$0")" && pwd)

case "$target" in
    linux-x64|linux-arm64)
        mozconfig_template="$here/mozconfig-$target"
        ;;
    android-arm64)
        mozconfig_template="$here/mozconfig-android-arm64"
        if [ -z "${ANDROID_NDK_HOME:-}" ] || [ ! -d "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin" ]; then
            echo "ANDROID_NDK_HOME must point to an Android NDK r27+ Linux installation" >&2
            exit 1
        fi
        ANDROID_NDK_HOME=$(cd "$ANDROID_NDK_HOME" && pwd)
        export ANDROID_NDK_HOME
        ;;
    *)
        echo "unsupported target: $target (expected linux-x64, linux-arm64, or android-arm64)" >&2
        exit 2
        ;;
esac

version=158.0.1
rust=1.95.0  # the Rust Firefox 158 is built with (taskcluster/kinds/toolchain/rust.yml)

case "$(uname -m)" in
    x86_64|amd64)
        rust_host=x86_64-unknown-linux-gnu
        ;;
    aarch64|arm64)
        rust_host=aarch64-unknown-linux-gnu
        ;;
    *)
        echo "unsupported build host architecture: $(uname -m)" >&2
        exit 1
        ;;
esac

export MOZBUILD_STATE_PATH="$work/mozbuild"
export CARGO_HOME="$work/cargo"
export RUSTUP_HOME="$work/rustup"
export PIP_CACHE_DIR="$work/pip-cache"
downloads="$work/downloads"
source="$work/src/firefox-$version"
objdir="$work/obj-$target-$version"
mkdir -p "$downloads" "$work/src" "$MOZBUILD_STATE_PATH" "$CARGO_HOME" "$RUSTUP_HOME" "$PIP_CACHE_DIR"

# Verify the Firefox source tarball against Mozilla's release SHA512SUMS.
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

# Install pinned Rust 1.95.0 in isolated CARGO_HOME / RUSTUP_HOME.
cargo_bin="$CARGO_HOME/bin"
if [ ! -x "$cargo_bin/rustup" ]; then
    installer="$downloads/rustup/rustup-init"
    mkdir -p "$downloads/rustup"
    base="https://static.rust-lang.org/rustup/dist/$rust_host"
    curl -fsSL -o "$installer" "$base/rustup-init"
    curl -fsSL -o "$installer.sha256" "$base/rustup-init.sha256"
    [ "$(sha256sum "$installer" | cut -d' ' -f1)" = "$(cut -d' ' -f1 < "$installer.sha256")" ] || { echo "rustup-init does not match its checksum" >&2; exit 1; }
    chmod +x "$installer"
    "$installer" -y --no-modify-path --profile minimal \
        --default-toolchain "$rust" --default-host "$rust_host"
fi
"$cargo_bin/rustup" toolchain install "$rust" --profile minimal --no-self-update
if [ "$target" = "android-arm64" ]; then
    "$cargo_bin/rustup" target add aarch64-linux-android --toolchain "$rust"
    export PATH="$cargo_bin:$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH"
else
    export PATH="$cargo_bin:$PATH"
fi
export RUSTUP_TOOLCHAIN="$rust"

# Extract and patch the Firefox source once.
if [ ! -f "$source/.ais2api-patched" ]; then
    rm -rf "$source"
    xz -dc "$tarball" | tar -xf - -C "$work/src"
    [ -f "$source/mach" ] || { echo "extracting the source tarball failed" >&2; exit 1; }
    for patch in "$here"/patches/*.patch; do
        patch -d "$source" -p1 --forward < "$patch"
    done
    touch "$source/.ais2api-patched"
fi

{
    printf '. "%s"\n' "$mozconfig_template"
    printf 'mk_add_options MOZ_OBJDIR=%s\n' "$objdir"
} > "$work/mozconfig-$target"
export MOZCONFIG="$work/mozconfig-$target"
cd "$source"
./mach configure < /dev/null
./mach build < /dev/null

sh "$here/package-unix.sh" "$target" "$objdir" "$MOZBUILD_STATE_PATH" "$work/dist/$target" "$tarball" "$archive"
