const W = 10;
const H = 20;
const EMPTY = "#24262A";
const COLORS = [EMPTY, "#30C7E8", "#FFD60A", "#AF52DE", "#34C759", "#FF3B30", "#0A84FF", "#FF9500"];
const SHAPES = [
  [[[0,1],[1,1],[2,1],[3,1]], [[2,0],[2,1],[2,2],[2,3]], [[0,2],[1,2],[2,2],[3,2]], [[1,0],[1,1],[1,2],[1,3]]],
  [[[1,0],[2,0],[1,1],[2,1]], [[1,0],[2,0],[1,1],[2,1]], [[1,0],[2,0],[1,1],[2,1]], [[1,0],[2,0],[1,1],[2,1]]],
  [[[1,0],[0,1],[1,1],[2,1]], [[1,0],[1,1],[2,1],[1,2]], [[0,1],[1,1],[2,1],[1,2]], [[1,0],[0,1],[1,1],[1,2]]],
  [[[1,0],[2,0],[0,1],[1,1]], [[1,0],[1,1],[2,1],[2,2]], [[1,1],[2,1],[0,2],[1,2]], [[0,0],[0,1],[1,1],[1,2]]],
  [[[0,0],[1,0],[1,1],[2,1]], [[2,0],[1,1],[2,1],[1,2]], [[0,1],[1,1],[1,2],[2,2]], [[1,0],[0,1],[1,1],[0,2]]],
  [[[0,0],[0,1],[1,1],[2,1]], [[1,0],[2,0],[1,1],[1,2]], [[0,1],[1,1],[2,1],[2,2]], [[1,0],[1,1],[0,2],[1,2]]],
  [[[2,0],[0,1],[1,1],[2,1]], [[1,0],[1,1],[1,2],[2,2]], [[0,1],[1,1],[2,1],[0,2]], [[0,0],[1,0],[1,1],[1,2]]]
];

Pocket.setBackground("#0B1018");
Pocket.rect(108, 2, 104, 204, "#111827", 4);
const boardGrid = Pocket.grid(110, 4, W, H, 10, 1, EMPTY);
const painted = [];
for (let y = 0; y < H; y += 1) {
  for (let x = 0; x < W; x += 1) {
    painted.push(-1);
  }
}
const scoreLabel = Pocket.text("SCORE 0000", 214, 8, 104, "#FFFFFF");
const statusLabel = Pocket.text("B: START", 214, 164, 104, "#9CA3AF");

let board = [];
let piece = 0;
let rotation = 0;
let px = 3;
let py = -1;
let score = 0;
let mode = "ready";
let nextDrop = 0;
let currentTime = 0;

function clearBoard() {
  board = [];
  for (let y = 0; y < H; y += 1) board.push([0,0,0,0,0,0,0,0,0,0]);
}

function fits(nx, ny, nr) {
  const blocks = SHAPES[piece][nr];
  for (let i = 0; i < 4; i += 1) {
    const x = nx + blocks[i][0];
    const y = ny + blocks[i][1];
    if (x < 0 || x >= W || y >= H || (y >= 0 && board[y][x])) return false;
  }
  return true;
}

function move(dx, dy) {
  if (!fits(px + dx, py + dy, rotation)) return false;
  px += dx; py += dy; render(); return true;
}

function rotate() {
  const nr = (rotation + 1) % 4;
  if (fits(px, py, nr)) rotation = nr;
  else if (fits(px - 1, py, nr)) { px -= 1; rotation = nr; }
  else if (fits(px + 1, py, nr)) { px += 1; rotation = nr; }
  else return;
  Pocket.tone(900, 12); render();
}

function spawn() {
  piece = Math.floor(Math.random() * 7); rotation = 0; px = 3; py = -1;
  if (!fits(px, py, rotation)) {
    mode = "gameover";
    Pocket.setText(statusLabel, "GAME OVER\nB: RETRY");
    Pocket.tone(170, 170);
  }
}

function lock() {
  const blocks = SHAPES[piece][rotation];
  let above = false;
  for (let i = 0; i < 4; i += 1) {
    const x = px + blocks[i][0];
    const y = py + blocks[i][1];
    if (y < 0) above = true; else board[y][x] = piece + 1;
  }
  let cleared = 0;
  for (let y = H - 1; y >= 0;) {
    let full = true;
    for (let x = 0; x < W; x += 1) if (!board[y][x]) full = false;
    if (!full) { y -= 1; continue; }
    board.splice(y, 1); board.unshift([0,0,0,0,0,0,0,0,0,0]); cleared += 1;
  }
  if (cleared) { score += cleared * cleared * 10; Pocket.tone(980 + cleared * 120, 70); }
  else Pocket.tone(330, 16);
  Pocket.setText(scoreLabel, `SCORE ${String(score).padStart(4, "0")}`);
  if (above) { mode = "gameover"; Pocket.setText(statusLabel, "GAME OVER\nB: RETRY"); }
  else spawn();
  render();
}

function softDrop(now) {
  if (mode !== "running") return;
  if (!move(0, 1)) lock();
  nextDrop = now + dropInterval();
}

function dropInterval() { return Math.max(130, 520 - Math.min(390, score * 2)); }

function startGame(now) {
  clearBoard(); score = 0; mode = "running"; nextDrop = now + dropInterval();
  Pocket.setText(scoreLabel, "SCORE 0000"); Pocket.setText(statusLabel, "B: PAUSE");
  spawn(); render(); Pocket.tone(660, 45);
}

function togglePause(now) {
  if (mode === "ready" || mode === "gameover") { startGame(now); return; }
  if (mode === "running") { mode = "paused"; Pocket.setText(statusLabel, "PAUSED\nB: RESUME"); return; }
  mode = "running"; nextDrop = now + dropInterval(); Pocket.setText(statusLabel, "B: PAUSE");
}

function render() {
  const view = [];
  for (let y = 0; y < H; y += 1) view.push(board[y].slice());
  if (mode === "running" || mode === "paused") {
    const blocks = SHAPES[piece][rotation];
    for (let i = 0; i < 4; i += 1) {
      const x = px + blocks[i][0]; const y = py + blocks[i][1];
      if (x >= 0 && x < W && y >= 0 && y < H) view[y][x] = piece + 1;
    }
  }
  for (let y = 0; y < H; y += 1) for (let x = 0; x < W; x += 1) {
    const index = y * W + x; const value = view[y][x];
    if (painted[index] !== value) { painted[index] = value; Pocket.setGridCell(boardGrid, x, y, COLORS[value]); }
  }
}

Pocket.button("^", 34, 36, 38, 34, () => { if (mode === "running") rotate(); }, "#475569");
Pocket.button("<", 8, 72, 38, 38, () => { if (mode === "running" && move(-1, 0)) Pocket.tone(520, 10); }, "#475569");
Pocket.repeatButton("v", 34, 110, 38, 38, () => softDrop(currentTime), "#475569", 180, 55);
Pocket.button(">", 60, 72, 38, 38, () => { if (mode === "running" && move(1, 0)) Pocket.tone(520, 10); }, "#475569");
Pocket.button("A", 242, 54, 42, 42, () => { if (mode === "running") rotate(); }, "#7C3AED");
Pocket.button("B", 270, 108, 42, 42, () => togglePause(currentTime), "#DC2626");

clearBoard(); render();
function frame(now) {
  currentTime = now;
  if (mode === "running" && now >= nextDrop) {
    if (!move(0, 1)) lock();
    nextDrop = now + dropInterval();
  }
}
