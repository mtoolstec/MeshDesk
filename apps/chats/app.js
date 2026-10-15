const device = Pocket.device();
const zh = device.language === "zh";
const dark = device.darkMode;
const tr = (en, cn) => zh ? cn : en;
const C = { bg: dark ? "#000000" : "#F2F2F7", card: dark ? "#1C1C1E" : "#FFFFFF", text: dark ? "#FFFFFF" : "#111111", muted: dark ? "#98989D" : "#71717A", segment: dark ? "#2C2C2E" : "#E3E3E8", blue: "#007AFF", green: "#34C759", danger: "#FF3B30" };
Pocket.setBackground(C.bg);
Pocket.setScrollable(false);

const PAGE_SIZE = 50;
const ROW_HEIGHT = 21, LIST_HEIGHT = 147, LIST_HEADER = 24, ROW_POOL = 9;
const persistent = typeof Pocket.chatHistory === "function";
let direct = false, contacts = [], contactIndex = 0;
let hasMore = true, pendingScroll = "bottom", renderedFirst = -1;
let layoutRows = [], contentHeight = LIST_HEADER;
const measuredHeights = new Map();
if (!persistent && !Pocket.readFile("legacy-messages.json")) {
  Pocket.writeFile("legacy-messages.json", JSON.stringify({ protocol: Pocket.communication().configurationProtocol, messages: Pocket.meshMessages() }));
}
let messages = [], revision = -1, selected = null, menuOpen = false, expanded = false;
let loadedProtocol = "";
const normal = [], menu = [], tools = [], confirmation = [], sounds = [];
let shortNames = false, clearing = null;
try { const preferences = JSON.parse(Pocket.readDataFile("preferences.json") || "{}"); shortNames = !!preferences.shortNames; } catch (_) {}
let toolsOpen = false, soundsOpen = false;
const soundLabels = [tr("Default", "默认"), tr("Chime", "铃声"), tr("Double", "双音"), tr("Soft", "柔和"), tr("Silent", "静音")];
const fittedText = [];
function fit(label, box) {
  fittedText.push([label, box]);
  Pocket.fitText(label, box);
}
const keep = (group, handle) => { group.push(handle); return handle; };
const show = (group, visible) => { for (let i = 0; i < group.length; i += 1) Pocket.setVisible(group[i], visible); };

const groupRect = keep(normal, Pocket.rect(10, 4, 74, 24, C.blue, 9));
const directRect = keep(normal, Pocket.rect(88, 4, 74, 24, C.segment, 9));
const groupText = keep(normal, Pocket.text(tr("Groups", "频道"), 10, 5, 74, "#FFFFFF"));
const directText = keep(normal, Pocket.text(tr("Direct", "私信"), 88, 5, 74, C.text));
fit(groupText, groupRect); fit(directText, directRect);
keep(normal, Pocket.hitArea(10, 4, 74, 24, () => selectMode(false)));
keep(normal, Pocket.hitArea(88, 4, 74, 24, () => selectMode(true)));
const groupsBadge = keep(normal, Pocket.rect(76, 2, 8, 8, C.danger, 4));
const directBadge = keep(normal, Pocket.rect(154, 2, 8, 8, C.danger, 4));
Pocket.setVisible(groupsBadge, false); Pocket.setVisible(directBadge, false);
const peerRect = keep(normal, Pocket.rect(166, 4, 112, 24, C.segment, 9));
const peerText = keep(normal, Pocket.text("# Public", 170, 5, 104, C.text));
fit(peerText, peerRect); keep(normal, Pocket.hitArea(166, 4, 112, 24, nextPeer));

const optionsText = keep(normal, Pocket.text("...", 282, 2, 28, C.blue));
Pocket.setTextAlign(optionsText, "center");
keep(normal, Pocket.hitArea(282, 4, 28, 24, openTools));

const messageList = keep(normal, Pocket.scrollArea(0, 32, 320, LIST_HEIGHT));
const content = Pocket.rect(0, 0, 320, LIST_HEADER, C.bg, 0);
Pocket.setParent(content, messageList);
const loadMoreText = Pocket.text(tr("Load more", "加载更多"), 10, 1, 296, C.blue);
Pocket.setTextAlign(loadMoreText, "center"); Pocket.setParent(loadMoreText, messageList);
const loadMoreArea = Pocket.hitArea(6, 0, 304, LIST_HEADER, loadMore);
Pocket.setParent(loadMoreArea, messageList);
const measurement = Pocket.text("", 0, 0, 296, C.text);
Pocket.setVisible(measurement, false);
const rowTexts = [], rowAreas = [], rowMessages = [];
// Reuse only the rows around the viewport: history never exhausts UI bindings.
for (let i = 0; i < ROW_POOL; i += 1) {
  const label = Pocket.text("", 0, 0, 300, C.text);
  const area = Pocket.longPressArea(6, 0, 304, ROW_HEIGHT, () => openMenu(i));
  Pocket.setSize(label, 300, ROW_HEIGHT);
  Pocket.setParent(label, messageList); Pocket.setParent(area, messageList);
  rowTexts.push(label); rowAreas.push(area);
}
const input = keep(normal, Pocket.textArea(10, 183, 188, 28, 120, true));
const expandRect = keep(normal, Pocket.rect(204, 183, 30, 28, C.segment, 9));
const expandIcon = keep(normal, Pocket.grid(205, 184, 14, 13, 2, 0, C.segment));
function paintExpandIcon() {
  const draw = (x, y) => Pocket.setGridCell(expandIcon, x, y, C.text);
  for (let i = 0; i <= 3; i += 1) {
    draw(2, 2 + i); draw(2 + i, 2);
    draw(11, 2 + i); draw(11 - i, 2);
    draw(2, 10 - i); draw(2 + i, 10);
    draw(11, 10 - i); draw(11 - i, 10);
  }
}
paintExpandIcon();
keep(normal, Pocket.hitArea(204, 183, 30, 28, expandEditor));
const sendRect = keep(normal, Pocket.rect(240, 183, 70, 28, C.blue, 10));
const sendText = keep(normal, Pocket.text(tr("Send", "发送"), 240, 187, 70, "#FFFFFF"));
fit(sendText, sendRect); const sendArea = keep(normal, Pocket.hitArea(240, 183, 70, 28, sendAction));

keep(menu, Pocket.rect(14, 20, 292, 112, C.card, 16));
const menuText = keep(menu, Pocket.text("", 28, 32, 264, C.text));
const menuButtons = [[24, C.blue, tr("Copy", "复制"), copySelected], [119, C.danger, tr("Delete", "删除"), deleteSelected], [214, C.segment, tr("Cancel", "取消"), closeMenu]];
for (let i = 0; i < menuButtons.length; i += 1) {
  const b = menuButtons[i]; const box = keep(menu, Pocket.rect(b[0], 72, 82, 38, b[1], 10));
  const label = keep(menu, Pocket.text(b[2], b[0], 80, 82, i === 2 ? C.text : "#FFFFFF")); fit(label, box);
  keep(menu, Pocket.hitArea(b[0], 72, 82, 38, b[3]));
}
show(menu, false);
keep(tools, Pocket.rect(10, 2, 300, 208, C.card, 14));
const storageText = keep(tools, Pocket.text(tr("Chat options", "聊天选项"), 24, 8, 270, C.text));
function toolButton(label, x, y, width, callback, color) {
  const box = keep(tools, Pocket.rect(x, y, width, 28, C.segment, 8));
  const text = keep(tools, Pocket.text(label, x, y + 3, width, color || C.blue));
  fit(text, box);
  keep(tools, Pocket.hitArea(x, y, width, 28, callback));
  return text;
}
toolButton(tr("Jump to latest", "回到最新消息"), 20, 36, 280, () => { closeTools(); resetPage(); refresh(true); });
const namesText = toolButton("", 20, 70, 138, toggleNames);
const soundText = toolButton(tr("Sound...", "通知声音..."), 162, 70, 138, openSounds);
toolButton(tr("Clear this chat...", "清空当前聊天..."), 20, 104, 280, askClear, C.danger);
toolButton(tr("Back to chat", "返回聊天"), 20, 172, 280, closeTools);
show(tools, false);
keep(confirmation, Pocket.rect(10, 32, 300, 138, C.card, 14));
const confirmText = keep(confirmation, Pocket.text("", 24, 42, 272, C.text));
for (const b of [[24, tr("Cancel", "取消"), cancelClear, C.segment], [166, tr("Clear", "清空"), startClear, C.danger]]) {
  const box = keep(confirmation, Pocket.rect(b[0], 122, 130, 32, b[3], 8));
  const text = keep(confirmation, Pocket.text(b[1], b[0], 128, 130, C.text)); fit(text, box);
  keep(confirmation, Pocket.hitArea(b[0], 122, 130, 32, b[2]));
}
show(confirmation, false);
keep(sounds, Pocket.rect(10, 2, 300, 208, C.card, 14));
const soundTitle = keep(sounds, Pocket.text("", 24, 8, 270, C.text));
const soundButtons = [];
for (let i = 0; i < soundLabels.length; i++) {
  const x = i % 2 ? 162 : 20, y = 36 + Math.floor(i / 2) * 34;
  const box = keep(sounds, Pocket.rect(x, y, 138, 28, C.segment, 8));
  const label = keep(sounds, Pocket.text("", x, y, 138, C.text)); fit(label, box);
  keep(sounds, Pocket.hitArea(x, y, 138, 28, () => chooseSound(i)));
  soundButtons.push({box, label});
}
const soundHint = keep(sounds, Pocket.text(tr("Tap to select and preview", "点击选择并试听"), 24, 138, 272, C.muted));
const soundBack = keep(sounds, Pocket.rect(20, 172, 280, 28, C.segment, 8));
const soundBackText = keep(sounds, Pocket.text(tr("Back to options", "返回选项"), 20, 172, 280, C.blue)); fit(soundBackText, soundBack);
keep(sounds, Pocket.hitArea(20, 172, 280, 28, closeSounds));
show(sounds, false);
function currentSound() { return typeof Pocket.notificationSound === "function" ? Pocket.notificationSound(direct) : 0; }
function resetSoundHint() { Pocket.setText(soundHint, tr("Tap to select and preview", "点击选择并试听")); }
function openSounds() { resetSoundHint(); soundsOpen = true; show(tools, false); show(sounds, true); render(); }
function closeSounds() { soundsOpen = false; show(sounds, false); show(tools, true); render(); }
function chooseSound(style) {
  if (typeof Pocket.notificationSound !== "function" || !Pocket.notificationSound(direct, style)) {
    Pocket.setText(soundHint, tr("Could not save sound", "声音设置保存失败")); return;
  }
  resetSoundHint();
  Pocket.previewNotificationSound(direct, style);
  render();
}
function openTools() { toolsOpen = true; show(normal, false); show(tools, true); render(); }
function closeTools() { toolsOpen = false; soundsOpen = false; show(sounds, false); show(tools, false); show(confirmation, false); show(normal, true); render(); }
function toggleNames() {
  shortNames = !shortNames;
  if (typeof Pocket.writeDataFile === "function") Pocket.writeDataFile("preferences.json", JSON.stringify({shortNames}));
  render();
}
function displayName(name, id) {
  if (!shortNames) return name || id || "";
  const hex = (id || "").replace(/^!/, "");
  return /^[0-9a-fA-F]{4,}$/.test(hex) ? hex.slice(-4).toLowerCase() : name || id || "";
}
function askClear() {
  if (clearing) return;
  show(tools, false); show(confirmation, true);
  Pocket.setText(confirmText, tr("Delete all text messages in ", "删除以下聊天的全部文字消息：") + (direct ? peerName() : tr("this public channel", "当前公共频道")) + tr("? Other chats and contacts are kept.", "？其他聊天和联系人保留。"));
}
function cancelClear() { show(confirmation, false); show(tools, true); }
function startClear() {
  if (direct && !peer()) { cancelClear(); return; }
  clearing = { protocol: Pocket.communication().configurationProtocol, direct, peerId: direct ? peer() : "", before: 0, ids: [], done: 0 };
  show(confirmation, false); show(tools, true);
}
function clearStep() {
  if (!clearing) return;
  if (clearing.protocol !== Pocket.communication().configurationProtocol) { clearing = null; return; }
  const state = persistent ? Pocket.chatStorage() : {ready:true,pending:0};
  if (!state.ready || state.pending) return;
  if (!clearing.ids.length) {
    const rows = persistent ? Pocket.chatHistory({direct:clearing.direct, peerId:clearing.peerId || undefined, limit:24, before:clearing.before, includeData:false}) : Pocket.meshMessages().filter(m=>m.direct===clearing.direct && (!clearing.direct || m.peer===clearing.peerId));
    if (!rows.length) { clearing = null; resetPage(); refresh(true); return; }
    clearing.before = rows[0].id;
    clearing.ids = rows.map(m=>persistent ? m.id : m.index);
    if (!persistent) clearing.ids.reverse();
  }
  for (let i = 0; i < 4 && clearing.ids.length; i++) {
    const id = clearing.ids[0];
    if (!(persistent ? Pocket.deleteChatMessage(id) : Pocket.meshDeleteMessage(id))) break;
    clearing.ids.shift(); clearing.done++;
  }
}

function refreshContacts() {
  const selectedPeer = peer();
  contacts = persistent ? Pocket.chatPeers() : Pocket.meshContacts().map(name => ({ id: name, name }));
  for (const m of messages) if (m.direct && m.peer) {
    const id = m.peerId || m.peer;
    if (!contacts.some(p => p.id === id)) contacts.push({ id, name: m.peer });
  }
  const index = contacts.findIndex(p => p.id === selectedPeer); contactIndex = index >= 0 ? index : 0;
}
function peer() { return contacts.length ? contacts[contactIndex].id : ""; }
function peerName() { return contacts.length ? contacts[contactIndex].name : ""; }
function resetPage() { messages = []; layoutRows = []; contentHeight = LIST_HEADER; measuredHeights.clear(); hasMore = true; pendingScroll = "bottom"; renderedFirst = -1; }
function history(cursor) {
  const options = { direct, limit: PAGE_SIZE, before: cursor, includeData: false };
  if (direct) options.peerId = peer();
  return Pocket.chatHistory(options);
}
function selectMode(value) { if (!menuOpen) { direct = value; Pocket.markChatRead(direct); refreshContacts(); contactIndex = 0; resetPage(); refresh(true); } }
function toggleMode() { selectMode(!direct); }
function nextPeer() { if (!menuOpen && direct && contacts.length) { contactIndex = (contactIndex + 1) % contacts.length; Pocket.markChatRead(true); resetPage(); refresh(true); } }
function loadMore() {
  if (!persistent || menuOpen || !hasMore || !messages.length) return;
  const y = Pocket.getScrollY(messageList);
  const rows = history(messages[0].id);
  hasMore = rows.length === PAGE_SIZE;
  if (rows.length) {
    const previousHeight = contentHeight;
    messages = rows.concat(messages);
    rebuildLayout();
    pendingScroll = Math.max(0, y) + contentHeight - previousHeight;
  }
  render();
}
function visibleMessages() {
  return persistent ? messages : messages.filter(m => m.direct === direct && (!direct || (peer() && m.peer === peer())));
}
function lineText(m) {
  return (!direct && !m.outgoing && m.peer ? displayName(m.peer, m.peerId) + ": " : "") + messageText(m);
}
function rebuildLayout() {
  let y = LIST_HEADER;
  layoutRows = visibleMessages().map(m => {
    const line = lineText(m);
    let height = measuredHeights.get(line);
    if (height === undefined) {
      height = Math.max(ROW_HEIGHT, Pocket.wrapText(measurement, line, 296, -6) - 6);
      measuredHeights.set(line, height);
    }
    const row = {m, line, y, height}; y += height; return row;
  });
  contentHeight = y;
}
function firstVisibleRow() {
  const y = Pocket.getScrollY(messageList);
  let low = 0, high = layoutRows.length;
  while (low < high) {
    const mid = (low + high) >> 1, row = layoutRows[mid];
    if (row.y + row.height <= y) low = mid + 1; else high = mid;
  }
  return low;
}
function renderRows() {
  const first = firstVisibleRow();
  renderedFirst = first;
  for (let i = 0; i < ROW_POOL; i += 1) {
    const row = layoutRows[first + i], m = row && row.m; rowMessages[i] = m;
    Pocket.setVisible(rowTexts[i], !!m); Pocket.setVisible(rowAreas[i], !!m);
    if (!m) continue;
    Pocket.setPosition(rowAreas[i], 6, row.y); Pocket.setSize(rowAreas[i], 304, row.height);
    Pocket.wrapText(rowTexts[i], row.line, 296, -6);
    Pocket.setPosition(rowTexts[i], 10, row.y - 3);
    Pocket.setColor(rowTexts[i], m.outgoing ? C.blue : (m.contact ? C.green : C.text));
    Pocket.setTextAlign(rowTexts[i], m.outgoing ? "right" : "left");
  }
}
function messageText(m) { return m.port && m.port !== 1 ? "Port " + m.port + ": " + m.dataHex : m.text; }
function refreshBadges() {
  const unread = Pocket.chatUnread();
  const visible = !toolsOpen && !menuOpen && !expanded;
  Pocket.setVisible(groupsBadge, visible && unread.groups);
  Pocket.setVisible(directBadge, visible && unread.direct);
}
function render() {
  Pocket.setChatView(direct, direct ? peer() : "", true);
  refreshBadges();
  Pocket.setColor(groupRect, direct ? C.segment : C.blue); Pocket.setColor(directRect, direct ? C.blue : C.segment);
  Pocket.setColor(groupText, direct ? C.text : "#FFFFFF"); Pocket.setColor(directText, direct ? "#FFFFFF" : C.text);
  Pocket.setText(peerText, direct ? (displayName(peerName(), peer()) || tr("No contacts", "暂无联系人")) : "# Public");
  Pocket.setText(namesText, shortNames ? tr("Names: short", "名称：简短") : tr("Names: full", "名称：完整"));
  Pocket.setText(soundTitle, direct ? tr("Direct notification sound", "私信通知声音") : tr("Group notification sound", "频道通知声音"));
  const style = currentSound();
  for (let i = 0; i < soundButtons.length; i++) {
    Pocket.setText(soundButtons[i].label, soundLabels[i]);
    Pocket.setColor(soundButtons[i].box, i === style ? C.blue : C.segment);
    Pocket.setColor(soundButtons[i].label, i === style ? "#FFFFFF" : C.text);
  }
  for (const pair of fittedText) Pocket.fitText(pair[0], pair[1]);
  Pocket.setText(loadMoreText, persistent && hasMore && messages.length ? tr("Load more", "加载更多") : tr("Start of chat", "没有更早的消息"));
  Pocket.setColor(loadMoreText, hasMore ? C.blue : C.muted);
  rebuildLayout();
  Pocket.setSize(content, 320, contentHeight);
  if (pendingScroll !== null) {
    // Hidden recycled rows must not extend the old scroll bounds after a reset.
    rowTexts.forEach(h => Pocket.setVisible(h, false));
    rowAreas.forEach(h => Pocket.setVisible(h, false));
    Pocket.scrollTo(messageList, pendingScroll === "bottom" ? Math.max(0, contentHeight - LIST_HEIGHT) : pendingScroll);
    pendingScroll = null;
  }
  renderRows();
}
function openMenu(row) { if (!rowMessages[row]) return; selected = rowMessages[row]; menuOpen = true; Pocket.setText(menuText, messageText(selected).slice(0, 40)); show(normal, false); show(menu, true); }
function closeMenu() { selected = null; menuOpen = false; show(menu, false); show(normal, true); render(); }
function copySelected() { if (selected) Pocket.setText(input, messageText(selected)); closeMenu(); }
function deleteSelected() { if (selected) { if (persistent) { if (Pocket.deleteChatMessage(selected.id)) messages = messages.filter(m => m.id !== selected.id); } else Pocket.meshDeleteMessage(selected.index); } closeMenu(); refresh(true); }
function expandEditor() {
  expanded = true; show(normal, false);
  Pocket.setTextAreaOneLine(input, false);
  Pocket.setPosition(input, 10, 4); Pocket.setSize(input, 300, 82); Pocket.setVisible(input, true);
  Pocket.setPosition(sendRect, 234, 8); Pocket.setPosition(sendText, 234, 12); Pocket.setPosition(sendArea, 234, 8);
  Pocket.setText(sendText, tr("Done", "完成")); Pocket.fitText(sendText, sendRect); Pocket.setVisible(sendRect, true); Pocket.setVisible(sendText, true); Pocket.setVisible(sendArea, true);
  Pocket.focus(input);
}
function collapseEditor() {
  expanded = false; Pocket.setPosition(input, 10, 183); Pocket.setSize(input, 188, 28);
  Pocket.setTextAreaOneLine(input, true);
  Pocket.setPosition(sendRect, 240, 183); Pocket.setPosition(sendText, 240, 187); Pocket.setPosition(sendArea, 240, 183);
  Pocket.setText(sendText, tr("Send", "发送")); show(normal, true); render();
}
function sendAction() { if (expanded) collapseEditor(); else send(); }
function send() { const value = (Pocket.getText(input) || "").trim(); if (!value || (direct && !peer())) return; if (Pocket.meshSend(direct, direct ? peer() : "", value)) Pocket.setText(input, ""); resetPage(); refresh(true); }
function refresh(force) {
  const protocol = Pocket.communication().configurationProtocol;
  if (protocol !== loadedProtocol) { loadedProtocol = protocol; resetPage(); force = true; }
  const next = Pocket.meshMessageRevision(); if (!force && next === revision) return; revision = next;
  if (persistent) {
    refreshContacts();
    const y = Pocket.getScrollY(messageList);
    const pinned = y >= contentHeight - LIST_HEIGHT - 8;
    const latest = history(0);
    if (!messages.length) { messages = latest; hasMore = latest.length === PAGE_SIZE; }
    else if (latest.length) {
      const oldestLatest = latest[0].id;
      messages = messages.filter(m => m.id < oldestLatest).concat(latest);
    } else { messages = []; hasMore = false; }
    if (pinned) pendingScroll = "bottom";
  }
  else { messages = Pocket.meshMessages(); refreshContacts(); }
  render();
}
function applyLaunchTarget() {
  if (typeof Pocket.takeChatTarget !== "function") return;
  const id = Pocket.takeChatTarget();
  if (!id) return;
  refreshContacts();
  const index = contacts.findIndex(p => p.id === id);
  if (index < 0) return;
  direct = true; contactIndex = index; Pocket.markChatRead(true); resetPage();
}
applyLaunchTarget(); refresh(true); let nextRefresh = 0;
function frame(now) {
  if (!toolsOpen && !menuOpen && !expanded) {
    const first = firstVisibleRow();
    if (first !== renderedFirst) renderRows();
  }
  if (now >= nextRefresh) { nextRefresh = now + 350; clearStep(); refresh(false); refreshBadges();
  if (persistent) { const state = Pocket.chatStorage();
    Pocket.setText(storageText, state.error || (!state.ready ? tr("SD unavailable", "SD 卡不可用") : clearing ? tr("Clearing... ", "正在清空... ") + clearing.done : state.pending ? tr("Saving...", "正在保存...") : tr("Chat options", "聊天选项")));
    Pocket.setColor(storageText, state.error || !state.ready ? C.danger : C.muted);
  }
} }
Pocket.log("Chats 1.8.0 persistent history ready");
