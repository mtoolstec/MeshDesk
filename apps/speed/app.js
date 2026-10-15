const d = Pocket.device();
const DARK = d.darkMode;
const BG = DARK ? "#05080A" : "#EEF3F6";
const PANEL = DARK ? "#0E1418" : "#FFFFFF";
const TEXT = DARK ? "#FFFFFF" : "#101418";
const MUTED = DARK ? "#8A9BA8" : "#6A7B85";
const CYAN = "#00D7E8";
const BLUE = "#38BDF8";

Pocket.setBackground(BG);
Pocket.rect(4, 2, 312, 214, PANEL, 20);

// Keep the dial dominant, like a compact vehicle instrument cluster. The
// firmware status bar already supplies the page title, clock and radio state.
// LVGL uses the shorter side as a round scale's diameter and aligns it at the
// object's left edge. Keep the gauge square so its true center is x=160.
const gauge = Pocket.gauge(54, 0, 212, 212, CYAN, BLUE, TEXT);
const speed = Pocket.number(88, 91, 144, 55, TEXT);
const unit = Pocket.text("km/h", 122, 148, 76, CYAN);
Pocket.setTextAlign(unit, "center");
const status = Pocket.text("WAITING FOR GPS", 18, 190, 284, MUTED);
Pocket.setTextAlign(status, "center");

let displayedSpeed = 0;
let nextFrame = 0;
function frame(now) {
  if (now < nextFrame) return;
  nextFrame = now + 120;
  const info = Pocket.device();
  if (!info.gpsEnabled) {
    Pocket.setText(status, "GPS OFF");
    Pocket.setText(speed, "--");
    Pocket.setGauge(gauge, 0);
    return;
  }
  if (!info.gpsFix) {
    Pocket.setText(status, "ACQUIRING GPS  |  " + info.gpsSatellites + " SAT");
    Pocket.setText(speed, "--");
    Pocket.setGauge(gauge, 0);
    return;
  }
  const target = Math.max(0, Math.min(199, Math.round(info.gpsSpeedKph)));
  const step = Math.max(1, Math.ceil(Math.abs(target - displayedSpeed) / 10));
  displayedSpeed += target > displayedSpeed ? step : (target < displayedSpeed ? -step : 0);
  if (Math.abs(target - displayedSpeed) <= step) displayedSpeed = target;
  Pocket.setText(speed, String(displayedSpeed));
  Pocket.setGauge(gauge, displayedSpeed);
  Pocket.setText(status, "GPS LOCK  |  " + info.gpsSatellitesUsed + " SAT  |  " +
                       String(Math.round(info.gpsCourseDegrees) % 360).padStart(3, "0") + " deg");
}
frame(0);
