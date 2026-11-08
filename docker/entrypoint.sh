#!/bin/sh
# Copyright (c) 2026 ais2api contributors
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# Container entrypoint: create the first configuration in the data volume,
# then run the foreground service so Docker can forward signals to it.
set -eu

data=${AIS2API_DATA_DIR:-/data}
config=${AIS2API_CONFIG_FILE:-"$data/config.json"}
bind=${AIS2API_BIND:-0.0.0.0}
port=${AIS2API_PORT:-18451}
browser=${AIS2API_BROWSER_EXECUTABLE:-/usr/bin/chromium}

if [ ! -f "$config" ]; then
    # Published ports can reach the service with a private source address
    # (Docker Desktop, rootless Docker, docker-proxy), which the gateway treats
    # as a local network peer. Never start with the public default password:
    # unless AIS2API_ADMIN_KEY is set, init stores a random one instead.
    generated=
    if [ -z "${AIS2API_ADMIN_KEY+set}" ]; then
        generated=$(od -An -N24 -tx1 /dev/urandom | tr -d ' \n')
        AIS2API_ADMIN_KEY=$generated
        export AIS2API_ADMIN_KEY
    fi
    initialized=$(/app/bin/ais2api init \
        --config "$config" \
        --data-dir "$data" \
        --bind "$bind" \
        --port "$port" \
        --browser "$browser")
    if [ -n "$generated" ]; then
        # The key now lives in the config file; serve must read it from there.
        unset AIS2API_ADMIN_KEY
        echo "ais2api: generated management password: $generated" >&2
        echo "ais2api: it is stored as \"admin_key\" in $config; change it after the first sign-in." >&2
    fi
    case "$initialized" in
        *'"api_key_configured":false'*)
            echo "ais2api: set \"api_key\" in $config before using generation endpoints." >&2
            ;;
    esac
    case "$initialized" in
        *'"admin_password_default":true'*)
            echo "ais2api: the management password is still \"admin\"; change it on first sign-in or set AIS2API_ADMIN_KEY." >&2
            ;;
    esac
fi

# Camoufox lives in the data volume. Install it when the default backend or a
# stored account needs it and it is missing or outdated: about 1.3 GB on the
# first start. A failure leaves the service running, and not ready.
doctor=$(/app/bin/ais2api doctor --config "$config" || true)
case "$doctor" in
    *'"browser_exists":false'*)
        case "$doctor" in
            *'"camoufox":{'*)
                echo "ais2api: installing Camoufox into $data/runtime/camoufox" >&2
                if ! /app/bin/ais2api browser update camoufox --yes --config "$config" >&2; then
                    echo "ais2api: Camoufox setup failed; run \"ais2api browser update camoufox --yes\" in the container to retry." >&2
                fi
                ;;
        esac
        ;;
esac

exec /app/bin/ais2api serve --config "$config"
