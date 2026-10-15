# I²C verification (2026-10-15)

The device uses SDA GPIO47 / SCL GPIO48 at 100 kHz. Grove power was enabled.
The Scanner application and a separate native `@WIOI2CSCAN` sweep both returned
these eight 7-bit ACK addresses:

| Address | Interpretation | Evidence / limit |
|---|---|---|
| 0x14 | ACK; unidentified | Product-register read returned [0,255,255,255], not a validated GT911 ID |
| 0x18 | ES8311 audio codec | Board codec initialization and reference variant |
| 0x21 | TCA9535 I/O expander | BoardPins / reference variant |
| 0x22 | AW35615 USB-C controller | Reference variant's explicit AW35615 probe |
| 0x2C | LP5814 LED/backlight driver | Hardware initialization and TI data sheet |
| 0x34 | LP5814 broadcast address | TI data sheet table 7-5: broadcast address bits 0110100 |
| 0x48 | ADS1115 ADC | Hardware ADC initialization |
| 0x5D | GT911 touch | Configured board touch address; register 0x8140 returned [57,49,49,0] (ASCII 911) |

0x34 is an address used by the LP5814, not evidence of an additional chip.
The [TI LP5814 data sheet](https://www.ti.com/lit/ds/symlink/lp5814.pdf)
section 7.5.2 describes independent and broadcast addressing. Unknown ACKs stay
unidentified; the old unconditional 0x50 “NFC” label was removed. No 0x50 ACK was
seen in this run. An address scan is not a general-purpose silicon-ID detector.

The application scans 0x08–0x77 once, without the former duplicated 0x50 probe,
and shows ACK address count rather than chip count. Probes recover the bus
between addresses. The native sweep initially stalled on NACK paths without
that recovery; it now uses the same board recovery before each transaction.
This comparison verifies application enumeration against the bus, not an
independent electrical logic-analyzer measurement.

A temporary diagnostic app read the touch product register on-device and was
removed from SD after the check. 0x14 remains `unknown (ACK)` in the UI.
