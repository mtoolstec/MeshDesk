Pocket.setBackground("#F4F1FF");
Pocket.rect(12, 12, 296, 76, "#5E5CE6", 16);
Pocket.text("PocketJS on Wio Tracker L2", 28, 28, 260, "#FFFFFF");

const device = Pocket.device();
const status = Pocket.text(
  `JS is running from SD card\nBattery: ${device.batteryMillivolts} mV`,
  20, 106, 280, "#252238"
);

let taps = 0;
Pocket.button("Tap me", 92, 160, 136, 42, () => {
  taps += 1;
  Pocket.setText(status, `JS is running from SD card\nButton taps: ${taps}`);
  Pocket.tone(620 + Math.min(taps, 8) * 55, 35);
}, "#5E5CE6");

Pocket.log("started", JSON.stringify(device));
