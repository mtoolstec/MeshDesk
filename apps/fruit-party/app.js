Pocket.setFullscreen(true);
Pocket.setBackground("#FFF4E9");
Pocket.rect(0, 0, 320, 43, "#FFFBEF", 0);
Pocket.rect(0, 207, 320, 33, "#DBEECF", 0);
const score = Pocket.text("0", 18, 10, 55, "#684C42");
const title = Pocket.text("FRUIT PARTY", 83, 10, 154, "#684C42");
Pocket.setTextAlign(title, "center");
const timer = Pocket.text("0:30", 264, 10, 48, "#684C42");
Pocket.rect(20, 35, 280, 5, "#E9D9BF", 3);
const timeFill = Pocket.rect(20, 35, 280, 5, "#FFB46E", 3);
const baskets = [];
for (let i = 0; i < 3; i += 1)
  baskets.push(Pocket.rect(15 + i * 25, 215, 18, 17, "#C8DBB6", 8));

const fruitColors = ["#FF8F8F", "#FFCE75", "#A9D88B", "#BFA9F1", "#FFB298"];
const fruitParts = [
  [29, 0, 8, 15], [36, 6, 19, 9], [2, 12, 66, 60], [12, 21, 19, 8],
  [22, 41, 4, 6], [44, 41, 4, 6], [16, 51, 9, 6], [47, 51, 9, 6],
  [27, 53, 3, 3], [30, 56, 10, 3], [40, 53, 3, 3]
];
const fruits = [];
let mode = "ready";
let lastNow = 0;
let lastMoveAt = 0;
let endAt = 0;
let secondsLeft = 30;
let points = 0;
let hits = 0;
let specialIndex = -1;
let nextSpecialAt = 0;
let sliceStarted = 0;
let sliceUntil = 0;
let sliceIndex = -1;
let slicePhase = -1;
let bannerUntil = 0;

for (let i = 0; i < 3; i += 1) {
  const parts = [];
  for (let j = 0; j < fruitParts.length; j += 1) {
    const colors = ["#8B6648", "#8FD391", fruitColors[i], "#FFFFFF", "#59413D", "#59413D",
                    "#F7A9AE", "#F7A9AE", "#8C5050", "#8C5050", "#8C5050"];
    const part = fruitParts[j];
    parts.push(Pocket.rect(0, 0, part[2], part[3], colors[j], j === 2 ? 29 : 4));
  }
  const crown = [Pocket.rect(0, 0, 25, 10, "#FFF4B1", 5),
                 Pocket.rect(0, 0, 9, 7, "#FFF4B1", 4)];
  for (const part of crown) Pocket.setVisible(part, false);
  const area = Pocket.pressArea(0, 0, 74, 78, () => cutFruit(i));
  fruits.push({ x: 13 + i * 101, y: 55 + i * 26, kind: i, parts, crown, area,
                speed: 29 + i * 7, wakeAt: 0 });
}

const slice = [Pocket.rect(0, 0, 29, 44, "#FFB8B8", 14),
               Pocket.rect(0, 0, 29, 44, "#FFB8B8", 14),
               Pocket.rect(0, 0, 9, 9, "#FFE58A", 5),
               Pocket.rect(0, 0, 9, 9, "#FFE58A", 5),
               Pocket.rect(0, 0, 9, 9, "#FFE58A", 5)];
for (const part of slice) Pocket.setVisible(part, false);

const panel = [
  Pocket.rect(37, 52, 248, 149, "#DFD8C5", 20),
  Pocket.rect(33, 48, 252, 149, "#FFFBEF", 20),
  Pocket.rect(140, 54, 40, 40, "#FFB997", 20),
  Pocket.rect(150, 68, 4, 5, "#684C42", 2),
  Pocket.rect(166, 68, 4, 5, "#684C42", 2),
  Pocket.rect(153, 83, 14, 3, "#A95E61", 2)
];
const panelTitle = Pocket.text("TAP THE FRUIT!", 61, 99, 198, "#684C42");
const panelNumber = Pocket.number(112, 114, 96, 40, "#684C42");
Pocket.setText(panelNumber, "30");
const playButton = Pocket.rect(91, 160, 138, 29, "#FFB46E", 15);
const playLabel = Pocket.text("START", 109, 166, 102, "#684C42");
Pocket.setTextAlign(panelTitle, "center");
Pocket.setTextAlign(playLabel, "center");
panel.push(panelTitle, panelNumber, playButton, playLabel);
const startArea = Pocket.pressArea(91, 160, 138, 29, startRound);

function showPanel(visible) {
  for (const part of panel) Pocket.setVisible(part, visible);
  Pocket.setVisible(startArea, visible);
}

function showFruit(index, visible) {
  const fruit = fruits[index];
  for (const part of fruit.parts) Pocket.setVisible(part, visible);
  for (const part of fruit.crown) Pocket.setVisible(part, visible && index === specialIndex);
  Pocket.setVisible(fruit.area, visible);
}

function drawFruit(index) {
  const fruit = fruits[index];
  const x = fruit.x, y = Math.floor(fruit.y);
  for (let i = 0; i < fruit.parts.length; i += 1)
    Pocket.setPosition(fruit.parts[i], x + fruitParts[i][0], y + fruitParts[i][1]);
  Pocket.setPosition(fruit.crown[0], x + 24, y + 15);
  Pocket.setPosition(fruit.crown[1], x + 32, y + 10);
  Pocket.setPosition(fruit.area, x, y + 6);
}

function colorFruit(index) {
  const fruit = fruits[index];
  Pocket.setColor(fruit.parts[2], index === specialIndex ? "#FFD475" : fruitColors[fruit.kind]);
  for (const part of fruit.crown) Pocket.setVisible(part, !fruit.wakeAt && index === specialIndex);
}

function assignSpecial() {
  if (specialIndex >= 0 || hits < nextSpecialAt) return;
  for (let i = 0; i < fruits.length; i += 1) {
    const index = (hits + i) % fruits.length;
    if (fruits[index].wakeAt) continue;
    specialIndex = index;
    colorFruit(index);
    return;
  }
}

function drawSlice(phase) {
  if (sliceIndex < 0 || slicePhase === phase) return;
  slicePhase = phase;
  const fruit = fruits[sliceIndex];
  const spread = phase === 0 ? 0 : phase === 1 ? 10 : 21;
  const y = Math.floor(fruit.y);
  Pocket.setPosition(slice[0], fruit.x + 7 - spread, y + 23);
  Pocket.setPosition(slice[1], fruit.x + 36 + spread, y + 23);
  Pocket.setPosition(slice[2], fruit.x + 8 - spread, y + 4);
  Pocket.setPosition(slice[3], fruit.x + 32, y - 8);
  Pocket.setPosition(slice[4], fruit.x + 59 + spread, y + 7);
}

function hideSlice() {
  for (const part of slice) Pocket.setVisible(part, false);
  sliceIndex = -1;
  sliceUntil = 0;
  slicePhase = -1;
}

function startRound() {
  if (mode === "running") return;
  mode = "running";
  endAt = lastNow + 30000;
  lastMoveAt = lastNow;
  secondsLeft = 30;
  points = hits = 0;
  specialIndex = 2;
  nextSpecialAt = 0;
  bannerUntil = 0;
  Pocket.setText(score, "0");
  Pocket.setText(timer, "0:30");
  Pocket.setText(title, "FRUIT PARTY");
  Pocket.setSize(timeFill, 280, 5);
  for (const basket of baskets) Pocket.setColor(basket, "#C8DBB6");
  hideSlice();
  for (let i = 0; i < fruits.length; i += 1) {
    fruits[i].wakeAt = 0;
    fruits[i].kind = i;
    fruits[i].y = 55 + i * 26;
    colorFruit(i);
    drawFruit(i);
    showFruit(i, true);
  }
  showPanel(false);
  Pocket.tone(660, 45);
}

function finishRound() {
  mode = "over";
  Pocket.setText(timer, "0:00");
  Pocket.setSize(timeFill, 1, 5);
  Pocket.setText(title, "YUMMY!");
  hideSlice();
  for (let i = 0; i < fruits.length; i += 1) showFruit(i, false);
  Pocket.setText(panelTitle, "FRUIT STAR!");
  Pocket.setText(panelNumber, String(points));
  Pocket.setText(playLabel, "NEW GAME");
  showPanel(true);
  Pocket.tone(784, 60);
  Pocket.tone(988, 60);
  Pocket.tone(1176, 85);
}

function cutFruit(index) {
  if (mode !== "running" || fruits[index].wakeAt) return;
  const special = index === specialIndex;
  hits += 1;
  let earned = special ? 4 : 1;
  if (hits % 5 === 0) {
    earned += 5;
    Pocket.setColor(baskets[Math.min(2, Math.floor(hits / 5) - 1)], "#FFCA67");
  }
  points += earned;
  Pocket.setText(score, String(points));
  Pocket.setText(title, hits % 5 === 0 ? "FRUIT PARTY!" : special ? "GOLD! +4" : "YUM! +1");
  bannerUntil = lastNow + 700;
  Pocket.setColor(slice[0], fruitColors[fruits[index].kind]);
  Pocket.setColor(slice[1], fruitColors[fruits[index].kind]);
  sliceIndex = index;
  sliceStarted = lastNow;
  sliceUntil = lastNow + 390;
  slicePhase = -1;
  drawSlice(0);
  for (const part of slice) Pocket.setVisible(part, true);
  fruits[index].wakeAt = lastNow + 490;
  showFruit(index, false);
  if (special) {
    specialIndex = -1;
    nextSpecialAt = hits + 2;
  }
  if (hits % 5 === 0) {
    Pocket.tone(850, 55);
    Pocket.tone(1080, 85);
  } else if (special) {
    Pocket.tone(900, 50);
    Pocket.tone(1180, 75);
  } else Pocket.tone(620 + index * 110, 42);
  assignSpecial();
}

function frame(now) {
  lastNow = now;
  if (mode !== "running") return;
  const millisecondsLeft = endAt - now;
  if (millisecondsLeft <= 0) { finishRound(); return; }
  const remaining = Math.ceil(millisecondsLeft / 1000);
  if (remaining !== secondsLeft) {
    secondsLeft = remaining;
    Pocket.setText(timer, "0:" + String(remaining).padStart(2, "0"));
    Pocket.setSize(timeFill, Math.max(1, Math.ceil(280 * remaining / 30)), 5);
  }
  if (sliceUntil) {
    if (now >= sliceUntil) hideSlice();
    else drawSlice(now - sliceStarted < 110 ? 0 : now - sliceStarted < 220 ? 1 : 2);
  }
  if (bannerUntil && now >= bannerUntil) {
    Pocket.setText(title, "FRUIT PARTY");
    bannerUntil = 0;
  }
  if (now - lastMoveAt < 50) return;
  const elapsed = Math.min(150, Math.max(0, now - lastMoveAt));
  lastMoveAt = now;
  for (let i = 0; i < fruits.length; i += 1) {
    const fruit = fruits[i];
    if (fruit.wakeAt) {
      if (now < fruit.wakeAt) continue;
      fruit.wakeAt = 0;
      fruit.y = 55;
      fruit.kind = (fruit.kind + 1) % fruitColors.length;
      colorFruit(i);
      drawFruit(i);
      showFruit(i, true);
      continue;
    }
    fruit.y += fruit.speed * elapsed / 1000;
    if (fruit.y > 137) {
      fruit.y = 55;
      fruit.kind = (fruit.kind + 1) % fruitColors.length;
      colorFruit(i);
    }
    drawFruit(i);
  }
  assignSpecial();
}
