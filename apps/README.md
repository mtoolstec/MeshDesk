# Wio Tracker L2 SD App SDK v3

An app lives in `/apps/<id>/` on the SD card. Document and approved native
runtime manifests remain supported. SDK v3 also supports a sandboxed
`pocketjs` app whose `app.js` is executed from SD by QuickJS.

Document apps remain compatible with SDK v1:

```json
{
  "type": "document",
  "name": "Trail Notes",
  "name_zh": "行程记录",
  "icon": "map",
  "color": "#34C759",
  "title": "Today",
  "title_zh": "今天",
  "body": "Notes from the trail",
  "body_zh": "行程记录"
}
```

Games use the generic PocketJS sandbox. Their complete UI and gameplay live on
the SD card; firmware only exposes generic widgets, frame callbacks, device
information, and audio:

For timing-sensitive controls, `Pocket.pressArea(x, y, width, height, callback)`
fires when the finger first touches the area. `Pocket.hitArea(...)` fires after
release and remains suitable for ordinary buttons. Move a game's sprite in the
press callback when immediate visual feedback matters.

An app can claim the complete 320 x 240 display during initial evaluation:

```js
Pocket.setFullscreen(true);
```

Without this declaration, the firmware status bar remains visible and the app
content area is 320 x 220 at y=20. The home indicator floats over the content.

```json
{
  "type": "pocketjs",
  "name": "Dinosaur",
  "name_zh": "恐龙游戏",
  "icon": "game",
  "color": "#343434",
  "entry": "app.js"
}
```

The Tetris app implements its 10 x 20 board, pieces, collision, scoring,
soft-drop and pause/resume state machine in `/apps/tetris/app.js`:

```json
{
  "type": "pocketjs",
  "name": "Block Stack",
  "name_zh": "俄罗斯方块",
  "icon": "game",
  "color": "#0A84FF",
  "entry": "app.js"
}
```

For younger children, `/apps/bubble-pop/`, `/apps/whack-a-mole/`,
`/apps/memory-pairs/`, and `/apps/color-match/` provide large single-touch
targets. Bubble Pop and Mole Pop use short 30-second rounds.
`/apps/fruit-party/` adds moving fruit targets, `/apps/little-piano/` offers
guided and free music play, and `/apps/animal-puzzle/` uses two-tap tile swaps.

`/apps/sdr/` is a receive-only 2.4 GHz spectrum viewer for the ESP32-S3 radio.
It uses native `sdrStart(channel)`, `sdrTune(channel)`, `sdrSpectrum()`,
`sdrError()`, and `sdrStop()` PocketJS APIs. Its bounded I/Q snapshots and FFT
are implemented in `SdrService`; Wi-Fi pauses while the SDR app owns the PHY
and resumes when the app closes. Channel is limited to Wi-Fi channels 1–11.

Copy an app without reflashing firmware:

```bash
python3 scripts/install_sd_app.py apps/dinosaur --port /dev/cu.usbmodem144401
```

To update an app over Wi-Fi, use the device's Web Files credentials. The
installer asks for the password and verifies the files after the atomic swap:

```bash
python3 scripts/deploy_app.py apps/whack-a-mole --base-url http://DEVICE_IP/ --username USER
```

The reserved `/apps/gps/` package replaces the firmware GPS desktop page. If
that PocketJS package is absent or cannot start, the native GPS page remains
available as a fallback. This allows GPS UI updates from SD without reflashing.

The reserved `/apps/chats/` package similarly replaces the built-in Chats
page. PocketJS chat replacements can use `meshMessages`, `meshContacts`,
`meshSend`, and `meshDeleteMessage`; removing the package restores the native
Chats page after an app rescan.

PocketJS apps can use the shared I²C0 bus on GPIO47 (SDA) and GPIO48 (SCL):
`Pocket.i2cOpen()`, `i2cClose()`, `i2cScan()`, `i2cWrite(address, bytes)`,
`i2cRead(address, length)`, and `i2cWriteRead(address, bytes, length)`.
Call `i2cOpen()` when entering an I²C app; the runtime also disables the Grove
connector automatically when that app exits. Transfers are limited to 64 bytes
and use 7-bit addresses at 100 kHz.

The installer creates the SD directory and transfers every top-level app file
atomically over acknowledged 24-byte USB frames, then restarts the device so
the launcher rescans apps. Manifests are limited to 4096 bytes; individual app
files are limited to 2 MiB.

## Communication lifecycle

`Pocket.communication()` returns `{protocol, ready, error, available}`. Currently `available` contains `off`, `meshcore` and `meshtastic`. `Pocket.selectProtocol("off")` stops communication; `Pocket.selectProtocol("meshcore")` starts it. The selection survives reboot. An unavailable protocol throws an exception and leaves the active backend unchanged.

```js
Pocket.selectProtocol("off");
Pocket.log(JSON.stringify(Pocket.communication()));
Pocket.selectProtocol("meshcore");
Pocket.meshSend(false, "", "Hello");
```

Protocol selection affects the whole device and persists beyond the app lifetime. The system clock continues while communication is off. The `meshMessages`, `meshContacts`, and `meshSend` APIs use the selected communication backend.

### Radio presets

`Pocket.radioPresets()` returns MeshCore presets with index, name, frequencyMHz, bandwidthKHz, spreadingFactor and codingRate. `Pocket.selectRadioPreset(index)` saves and applies a preset, or throws for an invalid index/save failure. Selection also works while the protocol is off. These are MeshCore configuration parameters; they do not install another protocol backend.

## Portable communication and Meshtastic

- `Pocket.communication()` returns `protocol`, `configurationProtocol`, `ready`,
  `error` and `available` (`off`, `meshcore`, `meshtastic`).
- `Pocket.selectProtocol(name)` switches and persists the active protocol.
- `Pocket.radioPresets()` / `Pocket.selectRadioPreset(index)` configure the active
  family (or the last configured family while off). Frequencies use MHz.
- `Pocket.peers()` returns index, stable id, name, contact, hasPublicKey, lastSeen.
- `Pocket.meshSend(false, "", text)` queues a public text.
  `Pocket.meshSend(true, peerIdOrName, text)` queues a private text. Meshtastic
  requires a received peer public key. Return `true` means queued, not delivered.
- `Pocket.advertise()` queues MeshCore Advert or Meshtastic NodeInfo.
- `Pocket.meshtasticConfig()` returns role, signalKnown, rssi, snr, region, channel, modem, frequencyMHz,
  nodeNumber, tx, rx, rejected, queued, lastTxId and lastAckId.
- `Pocket.setMeshtasticRole("Client")` or `Pocket.setMeshtasticRole("Client Mute")`
  persists the role. Client uses bounded managed flooding; Client Mute sends and
  receives its own messages without relaying other nodes.
- `Pocket.setMeshtasticRegion("US")`, `Pocket.setMeshtasticFrequency(0)`
  (automatic), `Pocket.setMeshtasticChannel("", "AQ==")` (default primary PSK).
  Settings reject invalid regions/frequencies or changes during queued TX.

Meshtastic starts with US / LongFast / default primary channel, 906.875 MHz.
Identity, configuration, discovered peer names/public keys and contact flags persist.
Live last-seen times are in RAM. Contacts and text/data history persist under `/appdata/chats`; see [storage format and APIs](../docs/CHAT_STORAGE.md). Missing sender keys trigger
automatic NodeInfo exchange and bounded encrypted-message replay.
The Radio system app selects the communication protocol and radio settings; the standalone Mesh Test app has been removed.
Backend limits and the two-device test are documented in
`docs/COMMUNICATION_BACKENDS.md` and `docs/MESHTASTIC_TESTING.md`.

## Per-app persistent files

`Pocket.readDataFile(name)` and `Pocket.writeDataFile(name, text)` use
`/appdata/<app-id>/`, independent of `/apps/<app-id>/`. Files are limited to
64 KiB, with a temporary file and backup on replacement. Names allow letters,
digits, dots, hyphens and underscores (maximum 48 characters); paths are rejected.
The existing `readFile`/`writeFile` APIs still use the app installation folder.

### Scrollable areas

`Pocket.scrollArea(x, y, width, height)` creates a vertical viewport. Attach
content with `Pocket.setParent(child, area)`. `Pocket.getScrollY(area)` reads
the current vertical offset; `Pocket.scrollTo(area, y)` updates layout and
scrolls immediately to a nonnegative offset within the content bounds.
Chats uses these APIs for a recycled message list and 50-message history batches.

### Fitted button labels

`Pocket.fitText(labelHandle, rectangleHandle)` fits a label to the rectangle
with 4 px side padding, uses a single font line, ellipsizes overflow, and centers
it horizontally and vertically. Call again after moving/resizing the rectangle.
Chats uses this for tabs, peer names and action buttons; full names remain stored.

`Pocket.wrapText(label, text, width, lineSpacing)` sizes a label for its complete
wrapped text using the label's font, and returns its pixel height. Width is
20–320 pixels; additional line spacing is -10–20 pixels (default 0).
Chats uses -6 pixel spacing and measured row heights to display full messages
while keeping the list compact.

### Notification sounds

`Pocket.notificationSound(direct)` reads the device-wide type for Groups (`false`)
or Direct (`true`). `Pocket.notificationSound(direct, type)` saves it and returns
success. Types: 0 Default, 1 Chime, 2 Double, 3 Soft, 4 Silent. Choices persist in
NVS and apply to incoming mesh messages even when the app is closed, subject to
the Settings sound switches. `Pocket.previewNotificationSound(direct, type)`
previews without changing preferences; preview follows volume but bypasses the
sound switches. Playback uses the bounded asynchronous hardware tone queue.

### DeepSeek 配置

运行配置保存在 SD 卡 `/appdata/deepseek/config.json`，使用 `Pocket.readDataFile/writeDataFile`。网页配置页也写入此目录。旧 `/apps/deepseek/config.json` 在首次启动时迁移；新文件读回校验通过后，旧文件被替换为 `{}`，不会因写入失败丢失旧配置。配置仍是明文，请勿把 SD 数据或 API Key 提交到仓库。更新应用时保留 `/appdata`。

### 动态二维码

`Pocket.qrCode(text, x, y, size)` 创建黑白二维码并包含留白，返回控件句柄；`Pocket.setQrCode(handle, text)` 更新内容。文本上限 256 字节，尺寸限制 64–192 像素。二维码占一个控件，使用 `setVisible` 显隐。DeepSeek 根据实时设备 IP 生成配置链接，未联网时隐藏二维码；需要包含此 API 的固件。

## Desktop icons and app discovery

`icon` accepts `game`, `folder`, `message`, `map`, `settings`, `power`,
`warning`, `home`, `speed`, `cloud`, `radio`, or `file`. Cloud is drawn with
LVGL shapes and requires no SD image. Custom icons must be 32×32 PNG files
in the app directory. App IDs are at most 24 characters; hidden directories
(such as `.install-*` and `.backup-*`) are excluded from desktop discovery.

Web Files service controls and login settings are under Settings → Storage,
with a matching Control Center shortcut. The service preference persists
across reboots and starts again once Wi-Fi connects.

USB maintenance also supports `@WIOAPPREMOVE <id>`: it removes only the
selected `/apps/<id>` package, leaves `/appdata/<id>` intact, and rescans the
desktop. IDs must contain only lowercase letters, digits, hyphens, or underscores.

USB recovery uploads may also target `/appdata/` using the same acknowledged
`@WIOPUT` / `@WIOCHUNK` transfer. `@WIOFORMATSD FAT32` erases the SD card and
disables the radio; make and verify a backup before invoking it. After restoring
packages and appdata, reboot and use `@WIOCOMMSELECT meshtastic` or
`@WIOCOMMSELECT meshcore` to resume the selected protocol. The formatter chooses
cluster size automatically for the card capacity and suspends data writes.
