#!/usr/bin/env python3
"""Atomically deploy a PocketJS app through the device Web Files API."""

import argparse
import base64
import getpass
import json
from pathlib import Path
import re
import time
from urllib.parse import quote, urlencode
from urllib.request import Request, urlopen


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app_dir", type=Path)
    parser.add_argument("--base-url", required=True, help="Device URL, e.g. http://192.0.2.1/")
    parser.add_argument("--username", required=True)
    args = parser.parse_args()

    app_dir = args.app_dir
    app_id = app_dir.name
    if not app_dir.is_dir() or not re.fullmatch(r"[a-z0-9_-]+", app_id):
        parser.error("app_dir must be a directory with a lowercase PocketJS app id")
    manifest = app_dir / "app.json"
    entry = app_dir / "app.js"
    if not manifest.is_file() or not entry.is_file():
        parser.error("app_dir needs app.json and app.js")
    raw_manifest = manifest.read_bytes()
    if not 1 <= len(raw_manifest) <= 4096:
        parser.error("app.json must be 1–4096 bytes")
    data = json.loads(raw_manifest)
    if data.get("type") != "pocketjs" or data.get("entry", "app.js") != "app.js" or not data.get("name"):
        parser.error("app.json must describe a named PocketJS app with entry app.js")
    files = sorted(path for path in app_dir.iterdir() if path.is_file() and not path.name.startswith("."))
    for path in files:
        if not 1 <= path.stat().st_size <= 2 * 1024 * 1024:
            parser.error(f"{path.name} must be 1–2097152 bytes")

    password = getpass.getpass("Device password: ")
    authorization = "Basic " + base64.b64encode(f"{args.username}:{password}".encode()).decode()
    base_url = args.base_url.rstrip("/")

    def request(path: str, body: bytes | None = None, content_type: str | None = None) -> bytes:
        headers = {"Authorization": authorization}
        if content_type:
            headers["Content-Type"] = content_type
        with urlopen(Request(base_url + path, data=body, headers=headers), timeout=30) as response:
            return response.read()

    form = urlencode({"id": app_id}).encode()
    form_type = "application/x-www-form-urlencoded"
    staging = "/apps/.install-" + app_id
    request("/api/install/start", form, form_type)
    committed = False
    try:
        for path in files:
            boundary = "codex-wio-app-upload"
            header = (f'--{boundary}\r\nContent-Disposition: form-data; name="file"; '
                      f'filename="{path.name}"\r\nContent-Type: application/octet-stream\r\n\r\n').encode()
            body = header + path.read_bytes() + f"\r\n--{boundary}--\r\n".encode()
            request("/api/upload?path=" + quote(staging, safe=""), body,
                    "multipart/form-data; boundary=" + boundary)
            print("Uploaded", path.name)
        request("/api/install/commit", form, form_type)
        committed = True
        time.sleep(0.5)
        for path in files:
            remote = "/apps/" + app_id + "/" + path.name
            endpoint = "/api/file" if path.stat().st_size <= 256 * 1024 else "/api/download"
            for attempt in range(3):
                try:
                    actual = request(endpoint + "?path=" + quote(remote, safe=""))
                    break
                except OSError:
                    if attempt == 2:
                        raise
                    time.sleep(1)
            if actual != path.read_bytes():
                raise RuntimeError("Device file differs: " + remote)
        print("Installed and verified", app_id)
    except Exception:
        if not committed:
            request("/api/install/cancel", form, form_type)
        raise


if __name__ == "__main__":
    main()
