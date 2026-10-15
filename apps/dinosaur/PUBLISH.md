# Application Publish Record

This app targets the Wio Tracker L2 SD App SDK v3. Its complete UI and game
logic run from SD through the generic PocketJS sandbox.

## Package

- SD manifest path: `/apps/dinosaur/app.json`
- Package: `dinosaur`
- Version: `1.0.0`
- Architecture: Wio Tracker L2 / ESP32-S3 firmware runtime
- Installer: `scripts/install_sd_app.py`
- Debian package path: not applicable
- APPLaunch desktop path: not applicable
- Runtime entry: `/apps/dinosaur/app.js` (`type: pocketjs`)

## Source and Ownership

- Public source repository: TODO
- Expected uploader/owner: TODO
- Hide uploader email: `true`

## Listing

- Application name: Dinosaur Game
- One-line summary: Touch-to-jump offline dinosaur runner for Wio Tracker L2
- Description: Jump over obstacles, build a score, and tap to retry after a collision.
- Categories: Game, Offline

## Assets

- Launcher icon: built-in game glyph selected by `icon: game`
- Store PNG icon: TODO if a store package format is added
- Screenshots: TODO if external publication is requested

## Preflight

- [x] `app.json` is valid and below the 4096-byte parser limit.
- [x] USB serial installer completed with an acknowledged atomic SD write.
- [x] Device reboot discovered Trail Notes and Dinosaur Game as two SD apps.
- [x] No credentials, tokens, or private server details are embedded.
- [ ] User explicitly approved external submission (not requested).

## Submission Result

- Status: `not submitted`
- Submitted at: not applicable
- Portal message: not applicable
- Actions URL: not applicable
- Tracking or pull-request URL: not applicable
