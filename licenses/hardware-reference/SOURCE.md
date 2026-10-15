# Hardware reference

The local source reference is `seeed_wio_tracker_l2-wio-l2-dev-dvt`, a Meshtastic
firmware tree distributed under GNU GPL version 3. The supplied local copy has
no `.git` metadata; an upstream commit cannot be verified from this copy.

Reviewed files:
- `src/graphics/TFTDisplay.cpp` (SEEED_WIO_TRACKER_L2 branch)
- `variants/esp32s3/seeed_wio_tracker_L2/variant.h`
- `variants/esp32s3/seeed_wio_tracker_L2/variant.cpp`

The display/backlight and I²C recovery implementation in `src/Hardware.cpp`
follows that reference and is retained under GPL-3.0-only. The reference file
`TFTDisplay.cpp` has no separate copyright header; its full license is preserved
here as `LICENSE`. The local reference fingerprint and comparison findings are
recorded in `docs/HARDWARE_PROVENANCE.json`. No new authorship claim is made over
that reference implementation.
