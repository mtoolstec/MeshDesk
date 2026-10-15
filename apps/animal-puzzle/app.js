Pocket.setFullscreen(true);
Pocket.setBackground("#EFF9FF");
Pocket.rect(0, 0, 320, 41, "#FFF9EA", 0);
Pocket.rect(0, 208, 320, 32, "#DDF0D5", 0);
const score = Pocket.text("0", 17, 10, 37, "#60465B");
const banner = Pocket.text("TAP TWO PIECES", 62, 10, 172, "#60465B");
Pocket.setTextAlign(banner, "center");
const themes = [
  ["#DDA777", "#B9775B", "#F4ADB1", "#A56672", "#463645", "BEAR"],
  ["#F3C5D6", "#EAA8C4", "#FFB3B8", "#A56672", "#463645", "BUNNY"],
  ["#A9CDE9", "#83B3DC", "#F1ABB5", "#A56672", "#463645", "KITTEN"]
];
const slots = [[13, 45], [120, 45], [13, 126], [120, 126]];
const headParts = [[30, 24, 70, 53], [3, 24, 70, 53],
                   [30, 3, 70, 55], [3, 3, 70, 55]];
const pieceSpecs = [
  [[18, 5, 27, 29, 1], [70, 48, 6, 8, 4]],
  [[58, 5, 27, 29, 1], [28, 48, 6, 8, 4]],
  [[53, 20, 17, 10, 2], [82, 11, 18, 12, 3], [74, 31, 26, 4, 4]],
  [[34, 20, 17, 10, 2], [3, 11, 18, 12, 3], [3, 31, 26, 4, 4]]
];
const pieces = [];
const areas = [];
let board = [2, 0, 3, 1];
let selected = -1;
let mode = "playing";
let solved = 0;
let themeIndex = 0;
let lastNow = 0;
let sparkleUntil = 0;

for (let piece = 0; piece < 4; piece += 1) {
  const parts = [Pocket.rect(0, 0, 103, 77, "#FFFFFF", 12),
                 Pocket.rect(0, 0, 97, 71, "#E8F8FF", 10),
                 Pocket.rect(0, 0, headParts[piece][2], headParts[piece][3], themes[0][0], 13)];
  for (const spec of pieceSpecs[piece])
    parts.push(Pocket.rect(0, 0, spec[2], spec[3], themes[0][spec[4]], 5));
  pieces.push(parts);
}
for (let slot = 0; slot < 4; slot += 1) {
  const position = slots[slot];
  areas.push(Pocket.pressArea(position[0], position[1], 103, 77, () => tapSlot(slot)));
}

Pocket.rect(238, 48, 70, 92, "#FFF9EA", 15);
const previewBody = Pocket.rect(246, 59, 54, 54, themes[0][0], 27);
const previewLeftEar = Pocket.rect(247, 50, 18, 25, themes[0][1], 9);
const previewRightEar = Pocket.rect(281, 50, 18, 25, themes[0][1], 9);
Pocket.rect(260, 81, 4, 5, "#463645", 2);
Pocket.rect(282, 81, 4, 5, "#463645", 2);
Pocket.rect(270, 92, 8, 6, "#A56672", 3);
Pocket.rect(263, 101, 20, 3, "#A56672", 2);
const previewLabel = Pocket.text(themes[0][5], 243, 119, 61, "#60465B");
Pocket.setTextAlign(previewLabel, "center");
const badges = [];
for (let i = 0; i < 3; i += 1)
  badges.push(Pocket.rect(242 + i * 22, 159, 17, 17, "#D4DFD3", 9));

const sparkle = [];
for (let i = 0; i < 8; i += 1)
  sparkle.push(Pocket.rect(0, 0, 10, 10, i % 2 ? "#FFD66C" : "#FFB7CB", 5));
for (const part of sparkle) Pocket.setVisible(part, false);

const panel = [
  Pocket.rect(36, 54, 249, 145, "#D6DACA", 20),
  Pocket.rect(32, 50, 253, 145, "#FFF9EA", 20),
  Pocket.rect(139, 57, 42, 42, "#FFE08C", 21),
  Pocket.rect(149, 72, 4, 6, "#60465B", 2),
  Pocket.rect(167, 72, 4, 6, "#60465B", 2),
  Pocket.rect(153, 88, 14, 3, "#A56672", 2)
];
const panelTitle = Pocket.text("PUZZLE STAR!", 58, 110, 204, "#60465B");
const panelCount = Pocket.number(112, 128, 96, 36, "#60465B");
const nextButton = Pocket.rect(89, 164, 142, 29, "#FFC27E", 14);
const nextLabel = Pocket.text("NEXT ANIMAL", 99, 170, 122, "#60465B");
Pocket.setTextAlign(panelTitle, "center");
Pocket.setTextAlign(nextLabel, "center");
panel.push(panelTitle, panelCount, nextButton, nextLabel);
const nextArea = Pocket.pressArea(89, 164, 142, 29, nextPuzzle);

function showPanel(visible) {
  for (const part of panel) Pocket.setVisible(part, visible);
  Pocket.setVisible(nextArea, visible);
}
showPanel(false);

function drawPiece(piece, slot) {
  const x = slots[slot][0], y = slots[slot][1];
  Pocket.setPosition(pieces[piece][0], x, y);
  Pocket.setPosition(pieces[piece][1], x + 3, y + 3);
  Pocket.setPosition(pieces[piece][2], x + headParts[piece][0], y + headParts[piece][1]);
  for (let i = 0; i < pieceSpecs[piece].length; i += 1)
    Pocket.setPosition(pieces[piece][i + 3], x + pieceSpecs[piece][i][0],
                       y + pieceSpecs[piece][i][1]);
  Pocket.setColor(pieces[piece][0], slot === selected ? "#FFD369" : "#FFFFFF");
}

function drawBoard() {
  for (let slot = 0; slot < 4; slot += 1) drawPiece(board[slot], slot);
}
drawBoard();

function applyTheme() {
  const theme = themes[themeIndex];
  for (let piece = 0; piece < 4; piece += 1) {
    Pocket.setColor(pieces[piece][2], theme[0]);
    for (let i = 0; i < pieceSpecs[piece].length; i += 1)
      Pocket.setColor(pieces[piece][i + 3], theme[pieceSpecs[piece][i][4]]);
  }
  Pocket.setColor(previewBody, theme[0]);
  Pocket.setColor(previewLeftEar, theme[1]);
  Pocket.setColor(previewRightEar, theme[1]);
  Pocket.setText(previewLabel, theme[5]);
}

function showSparkle() {
  const offsets = [[7, 48], [46, 40], [186, 45], [213, 89],
                   [197, 181], [145, 198], [35, 179], [2, 111]];
  for (let i = 0; i < sparkle.length; i += 1) {
    Pocket.setPosition(sparkle[i], offsets[i][0], offsets[i][1]);
    Pocket.setVisible(sparkle[i], true);
  }
  sparkleUntil = lastNow + 1200;
}

function tapSlot(slot) {
  if (mode !== "playing") return;
  if (selected < 0) {
    selected = slot;
    Pocket.setColor(pieces[board[slot]][0], "#FFD369");
    Pocket.tone(620, 40);
    return;
  }
  if (selected === slot) {
    Pocket.setColor(pieces[board[slot]][0], "#FFFFFF");
    selected = -1;
    return;
  }
  const first = selected;
  const hold = board[first];
  board[first] = board[slot];
  board[slot] = hold;
  selected = -1;
  drawBoard();
  Pocket.tone(790, 45);
  if (board[0] !== 0 || board[1] !== 1 || board[2] !== 2 || board[3] !== 3) return;
  mode = "solved";
  solved += 1;
  Pocket.setText(score, String(solved));
  Pocket.setText(banner, "GREAT JOB!");
  if (solved <= badges.length) Pocket.setColor(badges[solved - 1], "#FFD369");
  Pocket.setText(panelCount, String(solved));
  showSparkle();
  showPanel(true);
  Pocket.tone(880, 60);
  Pocket.tone(1100, 70);
  Pocket.tone(1320, 90);
}

function nextPuzzle() {
  if (mode !== "solved") return;
  mode = "playing";
  themeIndex = (themeIndex + 1) % themes.length;
  board = solved % 2 ? [1, 3, 0, 2] : [2, 0, 3, 1];
  selected = -1;
  Pocket.setText(banner, "TAP TWO PIECES");
  for (const part of sparkle) Pocket.setVisible(part, false);
  sparkleUntil = 0;
  showPanel(false);
  applyTheme();
  drawBoard();
}

function frame(now) {
  lastNow = now;
  if (sparkleUntil && now >= sparkleUntil) {
    for (const part of sparkle) Pocket.setVisible(part, false);
    sparkleUntil = 0;
  }
}
