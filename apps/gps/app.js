const DARK = Pocket.device().darkMode;
const BG = DARK ? "#020A06" : "#EDF8F1";
const PANEL = DARK ? "#071A10" : "#DCEFE3";
const GRID_BG = DARK ? "#04130B" : "#D2E8DA";
const GRID_LINE = DARK ? "#0A522B" : "#82C29A";
const TEXT = DARK ? "#E9FFF1" : "#123A21";
const MUTED = DARK ? "#315B40" : "#A8C9B3";
const GREEN = "#2AF07D", ORANGE = "#FF9F0A", BLUE = "#45AFFF";
const PURPLE = "#BF5AF2", RED = "#FF453A", CYAN = "#32D6E8";
const N = 24, CELL = 5, MAX_ROWS = 16;

Pocket.setBackground(BG);
Pocket.rect(3, 4, 140, 202, PANEL, 13);
Pocket.rect(147, 4, 170, 202, PANEL, 13);
const radar = Pocket.grid(13, 9, N, N, CELL, 0, GRID_BG);

function baseColor(x, y) {
  const center = (N - 1) / 2, dx = x - center, dy = y - center;
  const radius = Math.sqrt(dx * dx + dy * dy);
  const ring = Math.abs(radius - 3.7) < .45 || Math.abs(radius - 7.5) < .45 || Math.abs(radius - 11) < .55;
  return ring || x === 11 || x === 12 || y === 11 || y === 12 ? GRID_LINE : GRID_BG;
}
for (let y = 0; y < N; y += 1)
  for (let x = 0; x < N; x += 1) Pocket.setGridCell(radar, x, y, baseColor(x, y));

const north = Pocket.text("N", 63, 5, 16, GREEN);
Pocket.setTextAlign(north, "center");
const fixState = Pocket.text("ACQUIRING", 9, 133, 128, GREEN);
Pocket.setTextAlign(fixState, "center");
const latitude = Pocket.text("LAT --", 8, 158, 130, TEXT);
const longitude = Pocket.text("LON --", 8, 178, 130, TEXT);
Pocket.setTextAlign(latitude, "center");
Pocket.setTextAlign(longitude, "center");

Pocket.text("TELEMETRY", 156, 10, 153, GREEN);
const altitudeLabel = Pocket.text("ALT --", 156, 31, 153, TEXT);
Pocket.text("SATELLITES", 156, 52, 153, TEXT);

const list = Pocket.scrollArea(152, 73, 160, 130);
const rows = [];
for (let i = 0; i < MAX_ROWS; i += 1) {
  const y = 6 + i * 22;
  const name = Pocket.text("", 0, y + 2, 47, TEXT);
  Pocket.setParent(name, list); Pocket.setPosition(name, 1, y + 2);
  const bars = [];
  for (let b = 0; b < 5; b += 1) {
    const bar = Pocket.rect(0, 0, 13, 8, MUTED, 2);
    Pocket.setParent(bar, list); Pocket.setPosition(bar, 49 + b * 14, y + 10); bars.push(bar);
  }
  const value = Pocket.text("", 0, 0, 36, TEXT);
  Pocket.setParent(value, list); Pocket.setPosition(value, 113, y + 2); Pocket.setTextAlign(value, "right");
  rows.push({name, bars, value});
  Pocket.setVisible(name, false); Pocket.setVisible(value, false);
  for (let b = 0; b < bars.length; b += 1) Pocket.setVisible(bars[b], false);
}

function satelliteIdentity(s) {
  let name = "GPS", color = GREEN, id = s.id;
  if (s.system === "L" || (id >= 65 && id <= 96)) { name = "GLO"; color = BLUE; id -= 64; }
  else if (id >= 193 && id <= 200) { name = "QZS"; color = RED; id -= 192; }
  else if (s.system === "B" || (id >= 201 && id <= 237)) { name = "BD"; color = ORANGE; if (id >= 201) id -= 200; }
  else if (s.system === "A" || (id >= 301 && id <= 336)) { name = "GAL"; color = PURPLE; if (id >= 301) id -= 300; }
  else if (id >= 33 && id <= 64) { name = "SBAS"; color = CYAN; }
  return {label: name + String(id).padStart(2, "0"), color};
}

let oldDots = [], nextUpdate = 0;
function frame(now) {
  if (now < nextUpdate) return;
  nextUpdate = now + 1000;
  const d = Pocket.device(), sats = d.gpsSatelliteInfo || [];
  for (let i = 0; i < oldDots.length; i += 1)
    Pocket.setGridCell(radar, oldDots[i][0], oldDots[i][1], baseColor(oldDots[i][0], oldDots[i][1]));
  oldDots = [];
  for (let i = 0; i < sats.length; i += 1) {
    const s = sats[i];
    if (!s.azimuth && !s.elevation) continue;
    const radius = 10.5 * (90 - s.elevation) / 90, angle = s.azimuth * Math.PI / 180;
    const x = Math.max(0, Math.min(23, Math.round(11.5 + Math.sin(angle) * radius)));
    const y = Math.max(0, Math.min(23, Math.round(11.5 - Math.cos(angle) * radius)));
    Pocket.setGridCell(radar, x, y, satelliteIdentity(s).color);
    oldDots.push([x, y]);
  }

  const used = d.gpsSatellitesUsed || 0, total = d.gpsSatellites || sats.length;
  Pocket.setText(fixState, !d.gpsEnabled ? "GPS OFF" : (d.gpsFix ? `3D FIX  ${used}/${total}` : `SEARCHING  ${used}/${total}`));
  Pocket.setText(latitude, d.gpsFix ? `LAT ${d.latitude.toFixed(4)}` : "LAT --");
  Pocket.setText(longitude, d.gpsFix ? `LON ${d.longitude.toFixed(4)}` : "LON --");
  Pocket.setText(altitudeLabel, d.gpsAltitudeValid ? `ALT ${d.gpsAltitudeMeters.toFixed(0)} m` : "ALT --");

  sats.sort((a, b) => b.snr - a.snr);
  for (let i = 0; i < rows.length; i += 1) {
    const visible = i < sats.length;
    Pocket.setVisible(rows[i].name, visible); Pocket.setVisible(rows[i].value, visible);
    for (let b = 0; b < 5; b += 1) Pocket.setVisible(rows[i].bars[b], visible);
    if (!visible) continue;
    const s = sats[i], identity = satelliteIdentity(s);
    const level = Math.max(0, Math.min(5, Math.ceil(s.snr / 10)));
    const active = s.snr >= 35 ? GREEN : (s.snr >= 20 ? ORANGE : BLUE);
    Pocket.setText(rows[i].name, identity.label); Pocket.setText(rows[i].value, String(s.snr));
    Pocket.setColor(rows[i].name, identity.color);
    for (let b = 0; b < 5; b += 1) Pocket.setColor(rows[i].bars[b], b < level ? active : MUTED);
  }
}
frame(0);
