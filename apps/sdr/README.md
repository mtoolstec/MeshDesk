# 2.4 GHz SDR receiver

This app uses the ESP32-S3 Wi-Fi radio's receive-only I/Q capture path. It
displays a relative-power waterfall centered on Wi-Fi channels 1–11. Entering
the app pauses Wi-Fi and the Web Files service; leaving it restores Wi-Fi.
MeshCore's separate SX1262 LoRa radio remains independent.

The native `SdrService` reserves the S3 RF capture SRAM, takes a bounded
1024-sample snapshot at approximately 16 MS/s, and computes a 256-point FFT
on-device. The scale is **relative dB**, not calibrated dBm. Bursts are short,
so this is a spectrum viewer rather than continuous wideband recording.
The waterfall displays all 64 spectrum bins in 4×4-pixel cells and retains
24 rows of recent snapshots.

Hardware findings: [ESPARGOS/esp-sdr](https://github.com/ESPARGOS/esp-sdr)
and [lozaning/ESP32SDR](https://github.com/lozaning/ESP32SDR).
