#!/usr/bin/env python3
# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Portable executable defaults and relocation smoke test; Python is dev-only.

Use an installed bin/ais2api executable. Optional --browser-bundle accepts a
clean bundle root, its browser-runtime directory, or its platform directory.
No browser or Google request is performed. Child output and secrets stay in memory.
--require-webui also checks embedded pages and their entry assets before and after relocation.
"""
from __future__ import annotations

import argparse
from html.parser import HTMLParser
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request


class Failure(Exception):
    """Label-only failure."""


def free_port() -> int:
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class WebAssets(HTMLParser):
    """Entry scripts and stylesheets referenced by the installed console."""

    def __init__(self) -> None:
        super().__init__()
        self.resources: dict[str, str] = {}

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        attributes = dict(attrs)
        if tag == "script" and attributes.get("src"):
            self.resources[attributes["src"]] = "text/javascript"
        elif tag == "link" and attributes.get("href"):
            relations = (attributes.get("rel") or "").split()
            if "stylesheet" in relations:
                self.resources[attributes["href"]] = "text/css"
            elif "modulepreload" in relations:
                self.resources[attributes["href"]] = "text/javascript"


class PortableSmoke:
    def __init__(self, executable: Path, workspace: Path, bundle: Path | None,
                 require_webui: bool = False) -> None:
        self.current = "initialize"
        self.checks: list[str] = []
        self.secrets: list[str] = []
        self.started = False
        self.port = 0
        self.require_webui = require_webui
        workspace = workspace.resolve()
        tools = workspace / ".tools"
        if tools.is_symlink() or tools.resolve().parent != workspace:
            raise Failure("unsafe_fixture_root")
        tools.mkdir(exist_ok=True)
        self.fixture_root = tools.resolve()
        self.directory = Path(tempfile.mkdtemp(prefix="portable-smoke-", dir=tools)).resolve()
        self.program = self.directory / "original program"
        self.program.mkdir()
        self.cwd = self.directory / "unrelated working directory"
        self.cwd.mkdir()
        self.environment = {key: value for key, value in os.environ.items()
                            if not key.upper().startswith("AIS2API_")}
        self.user_roots = []
        for name in ("HOME", "LOCALAPPDATA", "USERPROFILE", "XDG_CONFIG_HOME", "XDG_CACHE_HOME"):
            root = self.directory / "fake user" / name.lower()
            root.mkdir(parents=True)
            self.user_roots.append(root)
            self.environment[name] = str(root)
        executable = executable.resolve(strict=True)
        self.executable_name = executable.name
        shutil.copy2(executable, self.program / executable.name)
        # Windows distributions may carry shared dependencies alongside the binary.
        for dependency in executable.parent.glob("*.dll"):
            shutil.copy2(dependency, self.program / dependency.name)
        shutil.copy2(executable.parent / "runtime-manifest.json", self.program / "runtime-manifest.json")
        (self.program / "browser").mkdir()
        shutil.copy2(executable.parent / "browser" / "bridge.js", self.program / "browser" / "bridge.js")
        self.platform = "windows-x64" if os.name == "nt" else "linux-x64"
        self.has_bundle = bundle is not None
        if bundle:
            source = bundle.resolve(strict=True)
            if (source / "browser-runtime" / self.platform).is_dir():
                source = source / "browser-runtime" / self.platform
            elif (source / self.platform).is_dir():
                source = source / self.platform
            if not (source / ".ais2api-runtime.json").is_file():
                raise Failure("bundle_marker_missing")
            # Only a clean explicit bundle is copied, never the source program's data.
            shutil.copytree(source, self.program / "browser-runtime" / self.platform,
                            symlinks=True)
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())

    def check(self, label: str, condition: bool) -> None:
        self.current = label
        if not condition:
            raise Failure(label)
        self.checks.append(label)

    def no_secret(self, value: str) -> None:
        if any(secret and secret in value for secret in self.secrets):
            raise Failure("secret_redaction")

    def cli(self, *arguments: str) -> dict:
        # There are intentionally no --config or --data-dir flags here.
        process = subprocess.Popen(
            [str(self.program / self.executable_name), *arguments],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", errors="strict", cwd=self.cwd,
            env=self.environment,
        )
        try:
            stdout, stderr = process.communicate(timeout=20)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
            raise Failure("cli_timeout") from None
        self.no_secret(stderr)
        self.no_secret(stdout)
        if process.returncode:
            raise Failure(self.current)
        try:
            value = json.loads(stdout)
        except (ValueError, TypeError):
            raise Failure("machine_readable_cli") from None
        if not isinstance(value, dict):
            raise Failure("machine_readable_cli")
        return value

    def fetch(self, path: str, limit: int = 65536) -> tuple[int, str, bytes]:
        try:
            response = self.opener.open(f"http://127.0.0.1:{self.port}{path}", timeout=10)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            status = response.status
            content_type = response.headers.get_content_type()
            body = response.read(limit + 1)
        if len(body) > limit:
            raise Failure("http_response_too_large")
        self.no_secret(body.decode("utf-8"))
        return status, content_type, body

    def http(self, path: str) -> tuple[int, dict]:
        status, _, body = self.fetch(path)
        return status, json.loads(body)

    def webui(self, prefix: str) -> None:
        resources: dict[str, str] = {}
        for name, route in (("home", "/"), ("login", "/login")):
            label = prefix + "_webui_" + name
            self.current = label
            status, content_type, body = self.fetch(route)
            self.check(label, status == 200 and content_type == "text/html" and bool(body.strip()))
            page = WebAssets()
            page.feed(body.decode("utf-8"))
            page.close()
            self.check(label + "_entry_assets",
                       {"text/javascript", "text/css"}.issubset(page.resources.values()))
            resources.update(page.resources)
        origin = f"http://127.0.0.1:{self.port}"
        for index, (reference, expected_type) in enumerate(resources.items()):
            label = prefix + f"_webui_asset_{index}"
            url = urllib.parse.urlsplit(urllib.parse.urljoin(origin + "/", reference))
            # Only request the installed service, never a remote asset from HTML.
            self.check(label + "_local", (url.scheme, url.netloc) == ("http", f"127.0.0.1:{self.port}"))
            path = urllib.parse.urlunsplit(("", "", url.path, url.query, ""))
            self.current = label
            status, content_type, body = self.fetch(path, 4 * 1024 * 1024)
            self.check(label, status == 200 and content_type == expected_type and bool(body.strip()))

    def homes_empty(self, prefix: str) -> None:
        self.check(prefix + "_fake_homes_empty", all(not any(root.iterdir()) for root in self.user_roots))
        self.check(prefix + "_working_directory_empty", not any(self.cwd.iterdir()))

    def stop_and_verify(self) -> None:
        if not self.started:
            return
        try:
            self.cli("stop")
        except Exception:
            pass
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            try:
                if self.cli("status").get("running") is False:
                    # init requires the exclusive instance lock and refuses a live service.
                    result = self.cli("init")
                    if result.get("initialized") is True and "api_key" not in result:
                        with socket.socket() as probe:
                            probe.settimeout(0.3)
                            if probe.connect_ex(("127.0.0.1", self.port)) != 0:
                                self.started = False
                                return
            except Exception:
                pass
            time.sleep(0.1)
        raise Failure("service_exit_not_confirmed")

    def cycle(self, prefix: str) -> None:
        self.current = prefix + "_config_load"
        configuration = self.cli("config", "show").get("config", {})
        self.check(prefix + "_resolved_data_directory",
                   Path(configuration.get("data_dir", "")).resolve() == (self.program / "data").resolve())
        if self.has_bundle:
            binary = "chrome.exe" if os.name == "nt" else "chrome"
            expected = self.program / "browser-runtime" / self.platform / binary
            self.check(prefix + "_resolved_bundled_browser",
                       Path(configuration.get("browser_executable", "")).resolve() == expected.resolve())
        # A new installation starts on Camoufox, unless the package carries CloakBrowser.
        if "AIS2API_BROWSER_BACKEND" not in self.environment:
            self.check(prefix + "_new_installation_backend",
                       configuration.get("browser_backend") == ("cloakbrowser" if self.has_bundle else "camoufox"))
        self.check(prefix + "_offline_status", self.cli("status").get("running") is False)
        self.port = free_port()
        self.current = prefix + "_start"
        self.started = True  # A timeout may still leave the detached child running.
        self.check(prefix + "_start", self.cli("start", "--port", str(self.port)).get("running") is True)
        self.check(prefix + "_online_status", self.cli("status").get("running") is True)
        code, body = self.http("/health")
        self.check(prefix + "_health", code == 200 and body.get("status") == "alive")
        code, body = self.http("/ready")
        self.check(prefix + "_not_ready_without_accounts", code == 503 and body.get("ready") is False)
        self.check(prefix + "_no_accounts", self.cli("accounts", "list").get("accounts") == [])
        if self.require_webui:
            self.webui(prefix)
        self.current = prefix + "_stop"
        self.stop_and_verify()
        self.check(prefix + "_lock_released", self.cli("init").get("initialized") is True)
        self.homes_empty(prefix)
        log = self.program / "data" / "service.log"
        if log.exists():
            self.no_secret(log.read_text(encoding="utf-8", errors="replace"))
        self.check(prefix + "_secret_absent_from_service_log", True)

    def run(self) -> None:
        self.current = "portable_init"
        first = self.cli("init")
        self.check("init_has_no_secret", "api_key" not in first and first.get("api_key_configured") is False)
        secret = "synthetic-portable-config-key-not-a-real-credential"
        self.secrets.append(secret)
        self.check("config_next_to_executable", (self.program / "config.json").is_file())
        self.check("data_next_to_executable", (self.program / "data").is_dir())
        configuration = json.loads((self.program / "config.json").read_text(encoding="utf-8"))
        self.check("default_port", configuration.get("port") == 18451)
        self.check("initial_config_key_empty", configuration.get("api_key") == "")
        configuration["api_key"] = secret
        (self.program / "config.json").write_text(json.dumps(configuration), encoding="utf-8")
        self.check("persisted_data_directory_relative", configuration.get("data_dir") == "data")
        if self.has_bundle:
            browser = configuration.get("browser_executable", "")
            self.check("persisted_bundled_browser_relative", bool(browser) and not Path(browser).is_absolute())
        repeated = self.cli("init")
        self.check("init_does_not_output_configured_key", "api_key" not in repeated and repeated.get("api_key_configured") is True)
        self.check("init_preserves_configured_key", json.loads((self.program / "config.json").read_text(encoding="utf-8")).get("api_key") == secret)
        keys = self.cli("keys", "list")
        self.cycle("original")
        old_program = self.program
        relocated = self.directory / "relocated program with spaces"
        old_program.rename(relocated)
        self.program = relocated
        self.check("whole_program_moved", not old_program.exists() and relocated.is_dir())
        self.check("relocated_key_metadata_unchanged", self.cli("keys", "list") == keys)
        persisted = (self.program / "config.json").read_text(encoding="utf-8")
        self.check("relocated_config_key_preserved", json.loads(persisted).get("api_key") == secret)
        self.check("relocated_config_has_no_original_absolute_path", str(old_program) not in persisted)
        self.cycle("relocated")
        self.check("key_metadata_preserved_after_restarts", self.cli("keys", "list") == keys)

    def cleanup(self) -> bool:
        try:
            self.stop_and_verify()
            target = self.directory.resolve()
            if (target.parent != self.fixture_root or not target.name.startswith("portable-smoke-")
                    or self.directory.is_symlink()):
                return False
            shutil.rmtree(target)
            return True
        except Exception:
            # Preserve the fixture if service shutdown or ownership is uncertain.
            return False


def main() -> int:
    workspace = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--browser-bundle", type=Path)
    parser.add_argument("--require-webui", action="store_true",
                        help="fail unless embedded pages and entry scripts/styles survive relocation")
    args = parser.parse_args()
    smoke = None
    failure = None
    try:
        # Assign before __init__ so even failed setup retains cleanup ownership.
        smoke = PortableSmoke.__new__(PortableSmoke)
        smoke.__init__(args.exe, workspace, args.browser_bundle, args.require_webui)
        smoke.run()
    except Exception:
        failure = getattr(smoke, "current", "initialize")
    finally:
        clean = smoke.cleanup() if smoke and hasattr(smoke, "directory") else True
    if not clean:
        failure = "cleanup_not_confirmed"
    checks = getattr(smoke, "checks", [])
    report = {"ok": failure is None, "checks": checks, "count": len(checks), "cleanup": clean}
    if failure:
        report["failed"] = failure
    print(json.dumps(report, separators=(",", ":")))
    return int(failure is not None)


if __name__ == "__main__":
    sys.exit(main())
