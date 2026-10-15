Pocket.setBackground("#F7F7F2");
let running = false;
let y = 143;
let velocity = 0;
let obstacleX = 290;
let points = 0;
let lastStep = 0;

Pocket.pressArea(4, 4, 312, 196, () => {
  if (!running) reset();
  else if (y >= 141) { velocity = -17; y -= 14; drawDino(); Pocket.tone(760, 34); }
});
Pocket.rect(0, 174, 320, 2, "#6B7280", 0);
const dinoParts = [
  [Pocket.rect(38, 150, 23, 16, "#287D3C", 2), 38, 7],
  [Pocket.rect(51, 139, 18, 16, "#287D3C", 2), 51, -4],
  [Pocket.rect(63, 144, 9, 8, "#287D3C", 1), 63, 1],
  [Pocket.rect(31, 153, 10, 7, "#287D3C", 1), 31, 10],
  [Pocket.rect(27, 156, 7, 4, "#287D3C", 1), 27, 13],
  [Pocket.rect(42, 164, 5, 10, "#287D3C", 1), 42, 21],
  [Pocket.rect(55, 164, 5, 10, "#287D3C", 1), 55, 21],
  [Pocket.rect(58, 154, 8, 3, "#287D3C", 1), 58, 11],
  [Pocket.rect(62, 142, 3, 3, "#FFFFFF", 1), 62, -1],
  [Pocket.rect(63, 142, 1, 1, "#111111", 0), 63, -1]
];
const cactus = Pocket.rect(290, 147, 9, 27, "#34C759", 2);
const score = Pocket.text("00000", 222, 8, 86, "#111111");
const hint = Pocket.text("Tap to run / jump", 76, 68, 190, "#374151");

function reset() {
  running = true; y = 129; velocity = -17; obstacleX = 290; points = 0;
  drawDino();
  Pocket.setText(score, "00000"); Pocket.setText(hint, "");
}

function drawDino() {
  for (let i = 0; i < dinoParts.length; i += 1)
    Pocket.setPosition(dinoParts[i][0], dinoParts[i][1], y + dinoParts[i][2]);
}

function frame(now) {
  if (!running || now - lastStep < 16) return;
  lastStep = now; velocity += 1.15; y += velocity;
  if (y >= 143) { y = 143; velocity = 0; }
  obstacleX -= 3 + Math.min(3, Math.floor(points / 10));
  if (obstacleX < -10) {
    obstacleX = 320 + Math.floor(Math.random() * 78); points += 1;
    Pocket.setText(score, String(points).padStart(5, "0")); Pocket.tone(1120, 18);
  }
  drawDino();
  Pocket.setPosition(cactus, obstacleX, 147);
  if (obstacleX < 72 && obstacleX + 9 > 31 && y + 31 > 147) {
    running = false; Pocket.setText(hint, "Game over - tap to retry"); Pocket.tone(190, 150);
  }
}
