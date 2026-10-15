#!/usr/bin/env python3
"""Fetch the pinned QuickJS sources used by the Wio L2 Pocket runtime."""

from __future__ import annotations

import hashlib
import pathlib
import urllib.request


REVISION = "04be246001599f5995fa2f2d8c91a0f198d3f34c"
BASE_URL = f"https://raw.githubusercontent.com/bellard/quickjs/{REVISION}"
FILES = (
    "LICENSE",
    "cutils.c",
    "cutils.h",
    "dtoa.c",
    "dtoa.h",
    "libregexp-opcode.h",
    "libregexp.c",
    "libregexp.h",
    "libunicode-table.h",
    "libunicode.c",
    "libunicode.h",
    "list.h",
    "quickjs-atom.h",
    "quickjs-opcode.h",
    "quickjs.c",
    "quickjs.h",
)


def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[1]
    destination = root / "lib" / "QuickJS"
    destination.mkdir(parents=True, exist_ok=True)
    for name in FILES:
        payload = urllib.request.urlopen(f"{BASE_URL}/{name}", timeout=60).read()
        # ESP-IDF 4.4 does not provide the pthread/atomic host expected by the
        # desktop QuickJS build. Atomics are not part of the L2 app surface.
        if name == "quickjs.c":
            payload = payload.replace(
                b"#if !defined(__EMSCRIPTEN__)\n#define CONFIG_ATOMICS\n#endif",
                b"#if !defined(__EMSCRIPTEN__) && !defined(ESP32)\n#define CONFIG_ATOMICS\n#endif",
                1,
            )
            payload = payload.replace(
                b"#if !defined(__EMSCRIPTEN__)\n/* enable stack limitation */\n#define CONFIG_STACK_CHECK\n#endif",
                b"#if defined(ESP32)\n#undef CONFIG_STACK_CHECK\n#elif !defined(__EMSCRIPTEN__)\n/* enable stack limitation */\n#define CONFIG_STACK_CHECK\n#endif",
                1,
            )
            payload = payload.replace(
                b"#if !defined(__EMSCRIPTEN__)\n#define CONFIG_STACK_CHECK\n#endif",
                b"#if defined(ESP32)\n#undef CONFIG_STACK_CHECK\n#elif !defined(__EMSCRIPTEN__)\n#define CONFIG_STACK_CHECK\n#endif",
                1,
            )
            payload = payload.replace(
                b"#include \"dtoa.h\"\n",
                b"#include \"dtoa.h\"\n\n#ifndef CONFIG_VERSION\n#define CONFIG_VERSION \"2026-06-04\"\n#endif\n",
                1,
            )
            payload = payload.replace(
                b"#elif defined(__EMSCRIPTEN__)\n    return 0;",
                b"#elif defined(__EMSCRIPTEN__) || defined(ESP32)\n    return 0;",
                1,
            )
            payload = payload.replace(
                b"#if defined(_WIN32)\n    {\n        struct tm *tm;",
                b"#if defined(ESP32)\n    res = 0;\n#elif defined(_WIN32)\n    {\n        struct tm *tm;",
                1,
            )
        target = destination / name
        target.write_bytes(payload)
        print(f"{name}: {len(payload)} bytes sha256={hashlib.sha256(payload).hexdigest()[:16]}")
    print(f"QuickJS {REVISION} fetched into {destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
