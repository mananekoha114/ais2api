# Copyright (c) 2026 ais2api contributors
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# Build natively on the target architecture. Buildx selects the matching
# Debian base image for linux/amd64 or linux/arm64.
#
# Install the pinned vcpkg dependencies in their own layer. A source change
# then rebuilds only the gateway, not Boost, OpenSSL, curl or libarchive.
FROM debian:13-slim AS build

ARG BUILD_JOBS=4

ENV DEBIAN_FRONTEND=noninteractive \
    VCPKG_MAX_CONCURRENCY=${BUILD_JOBS}

# hadolint ignore=DL3008
RUN apt-get update && apt-get install -y --no-install-recommends \
      autoconf \
      automake \
      bison \
      build-essential \
      ca-certificates \
      cmake \
      curl \
      file \
      flex \
      git \
      libtool \
      nasm \
      ninja-build \
      nodejs \
      npm \
      perl \
      pkg-config \
      python3 \
      tar \
      unzip \
      zip \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

RUN set -eu; \
    case "$(uname -m)" in \
      x86_64|amd64) triplet=x64-linux ;; \
      aarch64|arm64) triplet=arm64-linux ;; \
      *) echo "Unsupported container architecture: $(uname -m)" >&2; exit 1 ;; \
    esac; \
    printf '%s\n' "$triplet" > /tmp/vcpkg-triplet

COPY vcpkg.json scripts/read-baseline.cmake /opt/vcpkg-manifest/
# hadolint ignore=DL3003
RUN baseline=$(cmake \
      -DAIS2API_MANIFEST=/opt/vcpkg-manifest/vcpkg.json \
      -P /opt/vcpkg-manifest/read-baseline.cmake) \
    && triplet=$(cat /tmp/vcpkg-triplet) \
    && git clone --filter=blob:none --no-checkout \
      https://github.com/microsoft/vcpkg.git /opt/vcpkg \
    && git -C /opt/vcpkg checkout --detach "$baseline" \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics \
    && cd /opt/vcpkg-manifest \
    && /opt/vcpkg/vcpkg install --triplet "$triplet" \
      --x-install-root=/opt/vcpkg-installed \
      --clean-after-build \
    && rm -rf /root/.cache/vcpkg /opt/vcpkg/downloads /opt/vcpkg/buildtrees /opt/vcpkg/packages

COPY . .

RUN triplet=$(cat /tmp/vcpkg-triplet) \
    && cmake -S . -B build/linux -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_TESTING=OFF \
      -DAIS2API_BUILD_WEBUI=ON \
      -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
      -DVCPKG_TARGET_TRIPLET="$triplet" \
      -DVCPKG_MANIFEST_INSTALL=OFF \
      -DVCPKG_INSTALLED_DIR=/opt/vcpkg-installed \
      -DCMAKE_INSTALL_PREFIX=/src/build/linux/install \
    && cmake --build build/linux --parallel "${BUILD_JOBS}" \
    && cmake --install build/linux

# The runtime image carries the tested CMake install tree and Debian Chromium.
# CloakBrowser is not included: its binary license does not permit
# redistribution. A private image or a read-only mount can provide it.
# Camoufox is not included either: it is about 1.3 GB, and only accounts on a
# Firefox backend need it. The image carries the packages it runs with (Xvfb
# among them), and the entrypoint installs Camoufox into the data volume the
# first time it is needed.
FROM debian:13-slim AS runtime

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    AIS2API_BIND=0.0.0.0 \
    AIS2API_DATA_DIR=/data

# hadolint ignore=DL3008
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates \
      chromium \
      curl \
      fontconfig \
      fonts-noto-cjk \
      fonts-noto-color-emoji \
      libasound2t64 \
      libdbus-1-3 \
      libegl1 \
      libgl1-mesa-dri \
      libgtk-3-0t64 \
      libx11-xcb1 \
      xvfb \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /data

LABEL org.opencontainers.image.title="ais2api" \
      org.opencontainers.image.description="Native AI Studio gateway with Gemini and OpenAI-compatible APIs" \
      org.opencontainers.image.source="https://github.com/mananekoha114/ais2api" \
      org.opencontainers.image.licenses="MPL-2.0"

WORKDIR /app
COPY --from=build /src/build/linux/install/ /app/
COPY docker/entrypoint.sh /usr/local/bin/ais2api-entrypoint
RUN chmod 0755 /usr/local/bin/ais2api-entrypoint

EXPOSE 18451
VOLUME ["/data"]

# hadolint ignore=DL3025
HEALTHCHECK --interval=30s --timeout=5s --start-period=20s --retries=3 \
  CMD curl -fsS "http://127.0.0.1:${AIS2API_PORT:-18451}/health" || exit 1

ENTRYPOINT ["/usr/local/bin/ais2api-entrypoint"]
