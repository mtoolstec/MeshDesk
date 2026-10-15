const d = Pocket.device();
const DARK = d.darkMode;
const BG = DARK ? "#02070B" : "#EAF7FA";
const PANEL = DARK ? "#071419" : "#DDF2F4";
const GRID = DARK ? "#183840" : "#A8D6DA";
const TEXT = DARK ? "#E8FCFF" : "#12343A";
const MUTED = DARK ? "#78AAB3" : "#517B82";
const COLORS = ["#00D7E8", "#34D399", "#FACC15", "#FB7185", "#A78BFA", "#F97316"];
const CONFIG_FILE = "config.js";
const LEGACY_LOCATION_FILE = "last-location.json";
const RANGE_NM = 200;
const MAP_X = 2;
const MAP_Y = 36;
const MAP_W = 100;
const MAP_H = 170;
const CX = 52;
const CY = 121;

Pocket.setBackground(BG);
const status = Pocket.text("Preparing radar...", 6, 3, 308, MUTED);

// Left third: north-up radar. Right two thirds: matching color-coded table.
Pocket.rect(MAP_X, MAP_Y, MAP_W, MAP_H, PANEL, 8);
Pocket.rect(CX - 1, MAP_Y, 2, MAP_H, GRID, 0);
Pocket.rect(MAP_X, CY - 1, MAP_W, 2, GRID, 0);
Pocket.rect(CX - 4, CY - 4, 8, 8, "#FFFFFF", 4);
Pocket.text("N", 47, MAP_Y, 16, MUTED);
Pocket.text("W", 4, CY - 8, 16, MUTED);
Pocket.text("E", 87, CY - 8, 16, MUTED);
Pocket.text("S", 47, 189, 16, MUTED);

const flightHeader = Pocket.text("FLIGHT", 123, 27, 64, MUTED);
const altitudeHeader = Pocket.text("ALT", 187, 27, 61, MUTED);
const speedHeader = Pocket.text("SPEED", 248, 27, 66, MUTED);
Pocket.setTextAlign(flightHeader, "center");
Pocket.setTextAlign(altitudeHeader, "center");
Pocket.setTextAlign(speedHeader, "center");

const items = [];
for (let i = 0; i < 6; i += 1) {
  const y = 49 + i * 26;
  const mapDot = Pocket.rect(0, 0, 8, 8, COLORS[i], 4);
  const row = Pocket.rect(108, y - 3, 208, 23, i % 2 ? PANEL : BG, 4);
  const key = Pocket.rect(113, y + 10, 7, 7, COLORS[i], 4);
  const flight = Pocket.text("", 123, y, 64, TEXT);
  const altitude = Pocket.text("", 187, y, 61, TEXT);
  const speed = Pocket.text("", 248, y, 66, TEXT);
  Pocket.setTextAlign(flight, "center");
  Pocket.setTextAlign(altitude, "center");
  Pocket.setTextAlign(speed, "center");
  items.push({mapDot, row, key, flight, altitude, speed});
  for (const handle of [mapDot, row, key, flight, altitude, speed]) Pocket.setVisible(handle, false);
}

function setItemVisible(item, visible) {
  for (const handle of [item.mapDot, item.row, item.key, item.flight, item.altitude, item.speed]) Pocket.setVisible(handle, visible);
}

function clearAircraft() {
  for (const item of items) setItemVisible(item, false);
}

function validLocation(value) {
  return value && Number.isFinite(Number(value.latitude)) && Number.isFinite(Number(value.longitude));
}

function readJson(name) {
  try { return JSON.parse(Pocket.readFile(name) || "null"); } catch (error) { return null; }
}

function loadSavedLocation() {
  const configured = readJson(CONFIG_FILE);
  if (validLocation(configured)) return {latitude: Number(configured.latitude), longitude: Number(configured.longitude)};
  const legacy = readJson(LEGACY_LOCATION_FILE);
  return validLocation(legacy) ? {latitude: Number(legacy.latitude), longitude: Number(legacy.longitude)} : null;
}

function saveLocation(location) {
  Pocket.writeFile(CONFIG_FILE, JSON.stringify({latitude: location.latitude, longitude: location.longitude, source: "gps"}));
}

function callsign(a) {
  return a.flight && String(a.flight).trim() ? String(a.flight).trim() : a.hex || "N/A";
}

function bearing(center, a) {
  if (Number.isFinite(Number(a.dir))) return Number(a.dir);
  if (!Number.isFinite(Number(a.lat)) || !Number.isFinite(Number(a.lon))) return 0;
  const p1 = center.latitude * Math.PI / 180;
  const p2 = Number(a.lat) * Math.PI / 180;
  const dl = (Number(a.lon) - center.longitude) * Math.PI / 180;
  return (Math.atan2(Math.sin(dl) * Math.cos(p2), Math.cos(p1) * Math.sin(p2) - Math.sin(p1) * Math.cos(p2) * Math.cos(dl)) * 180 / Math.PI + 360) % 360;
}

function render(aircraft, location, source) {
  clearAircraft();
  aircraft.sort((a, b) => (Number(a.dst) || 9999) - (Number(b.dst) || 9999));
  const shown = Math.min(items.length, aircraft.length);
  Pocket.setText(status, shown + " ac | " + source + " " + location.latitude.toFixed(4) + "," + location.longitude.toFixed(4) + " | " + RANGE_NM + "nm");
  for (let i = 0; i < shown; i += 1) {
    const a = aircraft[i];
    const angle = bearing(location, a) * Math.PI / 180;
    const radius = Math.min(RANGE_NM, Math.max(0, Number(a.dst) || 0)) / RANGE_NM;
    const x = Math.round(CX + Math.sin(angle) * radius * 43);
    const y = Math.round(CY - Math.cos(angle) * radius * 76);
    const item = items[i];
    Pocket.setPosition(item.mapDot, Math.max(4, Math.min(94, x - 4)), Math.max(40, Math.min(197, y - 4)));
    Pocket.setText(item.flight, callsign(a).slice(0, 8));
    Pocket.setText(item.altitude, Number.isFinite(Number(a.alt_baro)) ? Math.round(Number(a.alt_baro) / 100) + "hft" : "--");
    Pocket.setText(item.speed, Number.isFinite(Number(a.gs)) ? Math.round(Number(a.gs)) + "kt" : "--");
    setItemVisible(item, true);
  }
}

let sessionLocation = loadSavedLocation();
let persistedLocation = sessionLocation;
let nextFetch = 0;
let initialized = false;

function locationChanged(a, b) {
  return Math.abs(a.latitude - b.latitude) > 0.005 || Math.abs(a.longitude - b.longitude) > 0.005;
}

function frame(now) {
  if (!initialized) {
    initialized = true;
    nextFetch = now + 1200;
    return;
  }
  if (now < nextFetch) return;
  nextFetch = now + 45000;

  const info = Pocket.device();
  let location = sessionLocation;
  let source = "saved";
  if (info.gpsFix && Number.isFinite(Number(info.latitude)) && Number.isFinite(Number(info.longitude))) {
    const gps = {latitude: Number(info.latitude), longitude: Number(info.longitude)};
    // A live GPS fix is the most accurate center: use it for the radar now and
    // remember it as the fallback if the fix drops. Persist only on movement.
    location = gps;
    sessionLocation = gps;
    source = "GPS";
    if (!persistedLocation || locationChanged(gps, persistedLocation)) {
      saveLocation(gps);
      persistedLocation = gps;
    }
  }
  if (!location) {
    Pocket.setText(status, info.gpsEnabled ? "Waiting for GPS fix" : "GPS off | no saved position");
    clearAircraft();
    return;
  }

  Pocket.setText(status, "Updating | " + source + " " + location.latitude.toFixed(4) + "," + location.longitude.toFixed(4) + " | " + RANGE_NM + "nm");
  const url = "http://api.adsb.lol/v2/point/" + location.latitude.toFixed(4) + "/" + location.longitude.toFixed(4) + "/" + RANGE_NM;
  try {
    const body = Pocket.httpGet(url, 32768);
    const data = body ? JSON.parse(body) : null;
    if (!data || !Array.isArray(data.ac)) throw new Error("invalid response");
    render(data.ac, location, source);
  } catch (error) {
    Pocket.setText(status, "Flight service unavailable");
  }
}
