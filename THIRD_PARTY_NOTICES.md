# Third-party components

The combined firmware is distributed under GPL-3.0-only. Original work: Copyright (c) 2026 Wilson and contributors. Third-party components retain their licenses and notices; see LICENSE for the full GPLv3 text.

- MeshCore: MIT, Scott Powell / rippleradios.com. Pinned revision: `ecffe7306722bee29c491b537063adcec7e2b45f`. Preserve its `license.txt` when distributing sources or firmware. This ESP32 snapshot omits unused Nordic SDK headers with Nordic-specific restrictions; see `lib/MeshCore/UPSTREAM.md`.
- MeshtasticLite: BSD-3-Clause, revision `c143b82faf5d94e1bc29fdbcfa90b96c589f8e0a`. Preserve `lib/MeshtasticLite/LICENSE` and source notices; local patches are recorded in `UPSTREAM.md`. This protocol implementation does not vendor the GPL Meshtastic firmware.
- ArduinoJson: MIT, Benoit Blanchon. Pinned version 7.4.3; preserve its dependency license. Used for app data/contact snapshots.
- Crypto 0.4.0: MIT, Southern Storm Software, Pty Ltd. Source header notice preserved in `licenses/dependencies/Crypto/header-MIT.txt`; individual source files retain their notices.
- QuickJS: MIT, Fabrice Bellard and Charlie Gordon. License included in `lib/QuickJS/LICENSE`.
- ed25519: Orson Peters, zlib-style license. Included in `lib/ed25519/LICENSE`.
- audio-driver 0.2.1: GPLv3, Phil Schatzmann. `AudioBoard.h` explicitly declares GPLv3; its license is preserved in `licenses/dependencies/audio-driver/License.txt`. This GPLv3 dependency is compatible with the project’s GPLv3 distribution choice.
- LVGL QR generator: Project Nayuki, MIT, shipped with LVGL; its source header notice is preserved in `licenses/dependencies/qrcodegen-MIT.txt`.
- Arduino ESP32, RadioLib, Crypto, LVGL, LovyanGFX, RTClib, Adafruit libraries (including Arduino ESP32’s LGPL notices): retain the notices shipped with each PlatformIO dependency. Complete license and asset provenance review remains required before a binary release.
- Chinese UI font: `src/lv_font_zh_cn_14.c` records generation from LVGL’s SourceHanSansSC-Normal.otf. Its bundled notice is preserved in `licenses/fonts/SourceHanSansSC-LICENSE.txt`.
- Monochrome emoji fallback: `src/lv_font_emoji_14.c` is generated from Noto Emoji, Copyright 2013 Google LLC, under SIL OFL 1.1. License: `licenses/fonts/NotoEmoji-OFL.txt`; pinned source and generation steps: `docs/ASSET_PROVENANCE.md` and `scripts/generate_emoji_font.py`.
- LVGL Montserrat and Font Awesome fonts: bundled notices preserved in `licenses/fonts/`. Font data retains its font license, independently of application code.
- App images with unverified provenance have been removed and replaced with built-in glyphs; removal hashes and historical limitations are recorded in `docs/ASSET_PROVENANCE.md`.

OpenStreetMap-derived map data and externally downloaded tiles are not covered by the project software license. Provider attribution and download permissions must be reviewed separately.

## Hardware reference provenance

The README names `seeed_wio_tracker_l2-wio-l2-dev-dvt` as a hardware reference. Its local checkout carries GPL-3.0. Hardware pin facts do not by themselves establish code derivation, but copied implementation code must not be relabeled MIT. A local comparison found matching display/backlight initialization structure and register sequences in `src/Hardware.cpp`. Evidence is in `docs/HARDWARE_PROVENANCE.json`. The project now retains this GPL-derived implementation under GPL-3.0-only; reference notices and license are preserved under licenses/hardware-reference.
