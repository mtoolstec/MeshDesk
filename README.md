# MeshDesk

Touchscreen mesh communicator firmware, with a device-independent project name
and a roadmap for multiple hardware platforms. The current implementation targets
the **Seeed Wio Tracker L2** (ESP32-S3), supports selectable MeshCore and
Meshtastic backends, and runs JavaScript applications from an SD card through
PocketJS.

**Status: development preview.** Core messaging and storage have been tested on
hardware. HTTPS certificate validation and the offline map provider policy remain
release blockers; see [Release readiness](docs/RELEASE_READINESS.md).

## Features

- A 320 × 240 touch interface with a launcher, Chats, Contacts, Radio, Settings,
  Files, GPS, Maps, and SD applications.
- MeshCore public messages, direct messages, contacts, and advertisements.
- A Meshtastic client backend with public channel text, NodeInfo, PKI direct
  messages, acknowledgments, and Client / Client Mute roles. This is a limited
  implementation; see [Communication backends](docs/COMMUNICATION_BACKENDS.md).
- Protocol, region, and radio presets selectable in Radio and through JavaScript.
  The selected configuration survives reboot.
- Contacts, chat history, and received protocol data persisted under
  `/appdata/chats`. Chats loads history in batches of up to 50 messages.
- PocketJS SDK v3, powered by QuickJS, with UI, storage, audio, device, and
  communication APIs. See the [application guide](apps/README.md).
- Web Files management and a USB serial SD application installer.
- WiFi and GPS time synchronization, shown by green and blue clock text.
- SD map tile rendering, manual or IP-derived coordinates, a remembered map
  center, and full-screen viewing.

## Hardware support

| Device | Status |
| --- | --- |
| Seeed Wio Tracker L2 | Current implementation; tested on hardware |
| M5Stack Tab5 | Planned port; not implemented or validated |

Additional devices will require board support and hardware validation.

## Hardware and tools

- Seeed Wio Tracker L2 with an ESP32-S3 and a 320 × 240 touchscreen.
- An SD card for applications, application data, and map tiles.
- A USB data connection for flashing and serial installation.
- Python 3, PlatformIO Core, and Node.js 22 or later for application tests.

The board configuration and initialization are based on the Wio Tracker L2 DVT
hardware reference. Preserve the reference notices in
[licenses/hardware-reference](licenses/hardware-reference).

## Build and flash

Install the host tools:

```sh
python3 -m pip install platformio pyserial
```

Build the firmware:

```sh
pio run -e wio_tracker_l2
```

Dependencies are pinned in `platformio.ini`. MeshCore, MeshtasticLite, QuickJS,
and ed25519 source snapshots are included in `lib/`; a neighboring checkout is
not required. PlatformIO downloads the toolchain and remaining dependencies on
the first build.

Flash through the device's built-in USB JTAG interface:

```sh
pio run -e wio_tracker_l2 -t upload
```

Alternatively, use the serial bootloader, replacing the port with your device:

```sh
pio run -e wio_tracker_l2 -t upload --upload-protocol esptool --upload-port PORT
```

## Install SD applications

After flashing the firmware, install an application over USB:

```sh
python3 scripts/install_sd_app.py apps/chats --port PORT
python3 scripts/install_sd_app.py apps/i2c-scanner --port PORT
```

Each application has its own directory under `/apps`. Persistent data lives
separately under `/appdata/<app-id>` and survives application updates. Reboot
after adding a new application so the launcher discovers it. See
[Chat storage](docs/CHAT_STORAGE.md) for retention and recovery behavior.

Enable and configure Web Files under **Settings → Storage → Web Files**, or open
it from Control Center. Control Center also provides Reboot.

## Radio configuration

Select **Off**, **MeshCore**, or **Meshtastic** in Radio. The backends do not run
simultaneously.

| Backend | Default configuration |
| --- | --- |
| MeshCore | EU/UK Narrow: 869.618 MHz, 62.5 kHz bandwidth, SF8, CR8 |
| Meshtastic | US / LongFast, automatic channel frequency (906.875 MHz with the default channel) |

Select settings appropriate for your region and match the other devices before
transmitting. Radio exposes region and modem presets; the JavaScript API also
supports a frequency override within the selected Meshtastic region.

## Validation

```sh
python3 scripts/validate_apps.py
node --test tests/*.test.js
c++ -std=c++17 -I tests/native_stubs -I include src/CommunicationService.cpp tests/communication_service.test.cpp -o /tmp/communication-test
/tmp/communication-test
```

The [CI workflow](.github/workflows/build.yml) also builds the firmware and runs
native chat storage, read-state, and Meshtastic protocol tests. Host tests use
simulated hardware; they do not replace radio, touchscreen, or SD testing on a
physical device. See [Meshtastic testing](docs/MESHTASTIC_TESTING.md) and
[I²C verification](docs/I2C_VERIFICATION.md) for hardware checks.

## Architecture

| Component | Responsibility |
| --- | --- |
| `main` | Initialization and service scheduling |
| `Hardware` | Display, touch, power, GPS, audio, and board peripherals |
| `CommunicationService` | Communication backend selection and lifecycle |
| `MeshService` / `MeshtasticService` | Protocol-specific communication |
| `ChatStore` | Persistent contacts and message history |
| `SystemClock` | Timekeeping independent of the radio backend |
| `PocketJsRuntime` | JavaScript application sandbox and Pocket APIs |
| `AppUI` | Launcher, native pages, navigation, and application lifecycle |
| `ConnectivityService` | WiFi, Web Files, SD operations, and map downloads |

Further modularization is described in the
[architecture review](docs/ARCHITECTURE_REVIEW.md).

## Known limitations

- PocketJS HTTPS requests currently skip certificate verification. DeepSeek uses
  this path for authenticated requests; its API key transport is not ready for a
  security-reviewed release. Proxy map TLS requests also need verification.
- Offline downloads currently target `tile.openstreetmap.org`. The offline use
  policy has not been cleared. A provider that explicitly permits the intended
  downloads, configuration, and attribution must be established before release.
- Meshtastic support is a client subset. Intermediate traceroute enrichment and
  full next-hop routing are not implemented.
- SD application data is plaintext. Do not publish SD backups, messages,
  credentials, private keys, or device configuration.
- Emoji use monochrome fallback glyphs; compound flags, skin tones, and ZWJ
  sequences are not composed.

## License and source provenance

MeshDesk is distributed under **GPL-3.0-only**. See [LICENSE](LICENSE) and
[Third-party notices](THIRD_PARTY_NOTICES.md). Third-party code and fonts retain
their respective licenses. Hardware reference provenance and retained notices are
included in the repository.

This public source baseline contains one initial commit. Earlier private
history, device backups, and generated firmware binaries are excluded. Current
asset provenance is recorded in [Asset provenance](docs/ASSET_PROVENANCE.md).
