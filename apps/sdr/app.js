const DARK = Pocket.device().darkMode;
const BG = DARK ? "#050B15" : "#EAF2FA";
const PANEL = DARK ? "#101F35" : "#D9E8F7";
const TEXT = DARK ? "#EAF7FF" : "#102A43";
const MUTED = DARK ? "#8FAAC2" : "#4B6479";
const OFF = DARK ? "#0A1626" : "#D0E0EF";
const COLORS = [OFF, "#153B7B", "#1D70B8", "#26B3BC", "#91E565", "#FFF16A", "#FFAC44", "#FF574F"];
const WIDTH = 64, HALF_WIDTH = 32, HEIGHT = 24;

Pocket.setBackground(BG);
Pocket.rect(5, 3, 310, 213, PANEL, 11);
Pocket.text("2.4G SDR / RX", 14, 7, 144, TEXT);
const channelLabel = Pocket.text("CH 1 / 2412 MHz", 160, 7, 145, TEXT);
Pocket.setTextAlign(channelLabel, "right");
const status = Pocket.text("Starting receiver...", 14, 28, 290, MUTED);
Pocket.button("-", 16, 49, 50, 29, () => changeChannel(-1), "#255784");
Pocket.button("+", 254, 49, 50, 29, () => changeChannel(1), "#255784");
Pocket.button("Retry", 95, 49, 130, 29, () => restart(), "#255784");
Pocket.text("-8 MHz", 32, 83, 90, MUTED);
const centerLabel = Pocket.text("2412 MHz", 112, 83, 96, TEXT);
Pocket.setTextAlign(centerLabel, "center");
const upper = Pocket.text("+8 MHz", 208, 83, 80, MUTED);
Pocket.setTextAlign(upper, "right");

const waterfallLeft = Pocket.grid(32, 99, HALF_WIDTH, HEIGHT, 4, 0, OFF);
const waterfallRight = Pocket.grid(160, 99, HALF_WIDTH, HEIGHT, 4, 0, OFF);
Pocket.text("Wi-Fi paused / RX only", 32, 198, 256, MUTED);

let channel = 1, running = false, nextSample = 0, row = 0;
function mhz() { return 2412 + (channel - 1) * 5; }
function showChannel() {
  Pocket.setText(channelLabel, "CH " + channel + " / " + mhz() + " MHz");
  Pocket.setText(centerLabel, mhz() + " MHz");
}
function restart() {
  Pocket.sdrStop();
  running = Pocket.sdrStart(channel);
  Pocket.setText(status, running ? "Live I/Q spectrum / 16 MHz span" : Pocket.sdrError());
  nextSample = 0;
}
function changeChannel(delta) {
  channel = Math.max(1, Math.min(11, channel + delta));
  showChannel();
  if (running && !Pocket.sdrTune(channel)) {
    running = false;
    Pocket.setText(status, Pocket.sdrError());
  }
}
function frame(now) {
  if (!running || now < nextSample) return;
  nextSample = now + 180;
  const bins = Pocket.sdrSpectrum();
  if (!bins) {
    Pocket.setText(status, Pocket.sdrError());
    running = false;
    return;
  }
  let floor = 100, peak = 0;
  for (let i = 0; i < bins.length; i += 1) {
    if (bins[i] < floor) floor = bins[i];
    if (bins[i] > peak) peak = bins[i];
  }
  for (let x = 0; x < WIDTH; x += 1) {
    const power = bins[x];
    const scale = Math.max(16, peak - floor);
    const index = Math.max(0, Math.min(7, Math.floor((power - floor) * 7 / scale)));
    Pocket.setGridCell(x < HALF_WIDTH ? waterfallLeft : waterfallRight,
                       x % HALF_WIDTH, row, COLORS[index]);
  }
  row = (row + 1) % HEIGHT;
  Pocket.setText(status, "Live I/Q spectrum / peak " + peak + " dB rel");
}

showChannel();
restart();
