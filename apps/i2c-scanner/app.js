Pocket.setBackground("#F2F2F7");
Pocket.rect(10, 8, 300, 198, "#FFFFFF", 14);
Pocket.text("I2C Scanner", 24, 20, 270, "#111111");
const status = Pocket.text("Starting Grove power...", 24, 58, 270, "#6B7280");
const resultArea = Pocket.scrollArea(20, 88, 280, 108);
const result = Pocket.text("Press Scan to probe I2C addresses", 0, 0, 258, "#111111");
Pocket.setParent(result, resultArea);
Pocket.setPosition(result, 6, 4);
// Board address hints, not a universal device identification database.
const ONBOARD = {24: "ES8311 codec", 33: "TCA9535 IO", 34: "AW35615 USB-C",
  44: "LP5814 LEDs", 52: "LP5814 broadcast", 72: "ADS1115 ADC", 93: "GT911 touch"};
let scanning = false;
let address = 0x08;
let found = [];
let scanStartedAt = 0;

function scan() {
  scanning = true;
  address = 0x08;
  found = [];
  scanStartedAt = 0;
  Pocket.setText(status, "Power settling...");
  Pocket.setText(result, "");
}

Pocket.button("Scan", 220, 14, 72, 30, scan, "#0A84FF");
Pocket.i2cOpen();
Pocket.setText(status, "Grove power ON");

function frame(now) {
  if (!scanning) return;
  if (!scanStartedAt) { scanStartedAt = now; return; }
  if (now - scanStartedAt < 800) return;
  if (address > 0x77) return;
  if (Pocket.i2cProbe(address)) {
    if (!found.includes(address)) found.push(address);
    const hex = "0x" + address.toString(16).toUpperCase().padStart(2, "0");
    Pocket.setText(status, "Found " + found.length + " · next 0x" + (address + 1).toString(16).toUpperCase().padStart(2, "0"));
    Pocket.setText(result, found.map((item) => {
      const itemHex = "0x" + item.toString(16).toUpperCase().padStart(2, "0");
      return itemHex + (ONBOARD[item] ? "  " + ONBOARD[item] : "  unknown (ACK)");
    }).join("\n"));
  }
  address += 1;
  if (address <= 0x77) {
    Pocket.setText(status, "Scanning 0x" + address.toString(16).toUpperCase().padStart(2, "0") + "...");
    return;
  }
  scanning = false;
  if (!found.length) {
    Pocket.setText(status, "Scan complete: none found");
    Pocket.setText(result, "Check SDA/SCL wiring and pull-ups");
    return;
  }
  Pocket.setText(status, "Complete: " + found.length + " ACK addresses");
  Pocket.setText(result, found.map((item) => {
    const hex = "0x" + item.toString(16).toUpperCase().padStart(2, "0");
    return hex + (ONBOARD[item] ? "  " + ONBOARD[item] : "  unknown (ACK)");
  }).join("\n"));
}
