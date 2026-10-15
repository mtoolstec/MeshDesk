const d = Pocket.device();
const DARK = d.darkMode;
const BG = DARK ? "#10131A" : "#F4F6FF";
const PANEL = DARK ? "#1B2130" : "#FFFFFF";
const TEXT = DARK ? "#F4F6FF" : "#1D2540";
const MUTED = DARK ? "#AAB4D0" : "#66708F";
const ACCENT = "#4D6BFE";
const CONFIG_FILE = "config.json";

Pocket.setBackground(BG);
Pocket.setScrollable(false);
Pocket.rect(12, 8, 296, 204, PANEL, 16);
Pocket.rect(12, 8, 296, 38, ACCENT, 14);
// One centered title line: the 14px font's baseline is placed at the
// vertical midpoint of the compact 38px blue header.
Pocket.text("DeepSeek 余额监控", 28, 18, 264, "#FFFFFF");
const status = Pocket.text("Checking...", 28, 56, 142, MUTED);
// Use ASCII: the device's compact font does not include hollow circles.
const loading = Pocket.text("Loading...", 174, 56, 118, MUTED);
Pocket.setTextAlign(loading, "right");
const balanceLabel = Pocket.text("当前余额", 28, 86, 100, MUTED);
const balance = Pocket.number(28, 98, 264, 48, ACCENT);
const detail = Pocket.text("", 28, 152, 264, MUTED);
Pocket.text("设备 IP", 28, 175, 76, MUTED);
const ipValue = Pocket.text(d.ipAddress || "未连接", 106, 175, 166, TEXT);
Pocket.setTextAlign(balance, "center");
Pocket.setTextAlign(ipValue, "right");

let setupHint = null, qrWidget = null, qrUrl = "";
function showSetup(visible) {
  const ip = Pocket.device().ipAddress;
  const url = ip && ip !== "0.0.0.0" ? "http://" + ip + "/app-config?id=deepseek" : "";
  if (!setupHint) setupHint = Pocket.text("", 28, 145, 264, TEXT);
  if (visible && url && typeof Pocket.qrCode === "function") {
    if (qrWidget === null) qrWidget = Pocket.qrCode(url, 115, 50, 90);
    else if (url !== qrUrl) Pocket.setQrCode(qrWidget, url);
    qrUrl = url;
  }
  const hasQr = qrWidget !== null && qrWidget >= 0 && !!url;
  Pocket.setText(setupHint, hasQr ? "扫码打开配置页" : url ? "请更新固件以显示二维码" : "请先连接 WiFi，再配置 API Key");
  Pocket.setVisible(setupHint, visible);
  if (qrWidget !== null && qrWidget >= 0) Pocket.setVisible(qrWidget, visible && hasQr);
  [status, balanceLabel, balance, detail].forEach(node => Pocket.setVisible(node, !visible));
}

let configMigrationFailed = false;
function readConfig() {
  configMigrationFailed = false;
  try {
    const stored = Pocket.readDataFile(CONFIG_FILE);
    if (stored !== null && stored !== undefined) return JSON.parse(stored);
    const legacy = Pocket.readFile(CONFIG_FILE);
    if (!legacy) return null;
    const config = JSON.parse(legacy);
    if (!config || typeof config.apiKey !== "string" || !config.apiKey) return null;
    if (!Pocket.writeDataFile(CONFIG_FILE, legacy) || Pocket.readDataFile(CONFIG_FILE) !== legacy) {
      configMigrationFailed = true;
      return null;
    }
    // Clear the old credential only after verifying the new file.
    Pocket.writeFile(CONFIG_FILE, "{}");
    return config;
  } catch (error) { return null; }
}

function setMessage(message, value, extra) {
  Pocket.setText(status, message);
  Pocket.setText(balance, value || "--");
  Pocket.setText(detail, extra || "");
}

function render(data) {
  Pocket.setVisible(loading, false);
  if (!data || !Array.isArray(data.balance_infos)) throw new Error("返回格式错误");
  const lines = data.balance_infos.map((item) => {
    const currency = item.currency || "";
    return currency + "  " + (item.total_balance || "0");
  });
  setMessage(data.is_available ? "Ready" : "Low balance", lines[0] || "No balance", lines.slice(1).join("   "));
}

let nextFetch = 0;
let first = true;
let loadingPhase = 0;
let nextLoadingFrame = 0;
function frame(now) {
  if (now >= nextLoadingFrame) {
    nextLoadingFrame = now + 400;
    loadingPhase = (loadingPhase + 1) % 4;
    Pocket.setText(loading, "Loading" + ["", ".", "..", "..."][loadingPhase]);
  }
  if (now < nextFetch) return;
  nextFetch = now + 60000;
  const config = readConfig();
  if (!config || !config.apiKey) {
    Pocket.setVisible(loading, false);
    if (configMigrationFailed) { setMessage("Migration failed", "--", "Check SD card and reopen app"); return; }
    showSetup(true);
    nextFetch = now + 2000;
    return;
  }
  showSetup(false);
  if (first) {
    first = false;
    nextFetch = now + 1200;
    setMessage("Checking...", "--", "");
    return;
  }
  Pocket.setVisible(loading, true);
  try {
    const base = String(config.baseUrl || "https://api.deepseek.com").replace(/\/$/, "");
    const body = Pocket.httpGet(base + "/user/balance", 16384, {authorization: "Bearer " + config.apiKey});
    const data = body ? JSON.parse(body) : null;
    if (!data) throw new Error("网络无响应");
    if (data.error) {
      Pocket.setVisible(loading, false);
      const message = data.error.message || data.error.type || "API 返回错误";
      setMessage("API error", "--", String(message).slice(0, 38));
      return;
    }
    render(data);
  } catch (error) {
    Pocket.setVisible(loading, false);
    setMessage("Request failed", "--", "Check key, network, or URL");
  }
}
