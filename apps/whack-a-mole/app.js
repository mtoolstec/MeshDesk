Pocket.setFullscreen(true);
Pocket.setBackground("#EAF8FF");
Pocket.rect(0, 0, 320, 37, "#FFF9E9", 0);
Pocket.rect(0, 211, 320, 29, "#D8EFCB", 0);
Pocket.rect(11, 218, 9, 9, "#FFB8CC", 5);
Pocket.rect(299, 217, 10, 10, "#FFCF76", 5);
const timer = Pocket.text("0:30", 22, 215, 58, "#5C3726");
Pocket.rect(84, 221, 210, 8, "#BADBB3", 4);
const timeFill = Pocket.rect(84, 221, 210, 8, "#FFBD74", 4);

const score = Pocket.text("0", 16, 8, 58, "#5C3726");
const mascot = Pocket.rect(76, 4, 30, 30, "#FFD872", 15);
Pocket.rect(84, 14, 3, 4, "#513923", 2);
Pocket.rect(95, 14, 3, 4, "#513923", 2);
Pocket.rect(85, 22, 2, 2, "#E67170", 1);
Pocket.rect(88, 24, 10, 2, "#E67170", 1);
Pocket.rect(99, 22, 2, 2, "#E67170", 1);
const banner = Pocket.text("MOLE POP", 107, 8, 119, "#5C3726");
Pocket.setTextAlign(banner, "center");
const progress = [];
for (let i = 0; i < 5; i += 1)
  progress.push(Pocket.rect(236 + i * 15, 11, 11, 11, "#E5C79F", 6));

const holes = [];
const moles = [];
const sparks = [];
const reaction = [];
let active = -1;
let golden = false;
let hits = 0;
let points = 0;
let spawns = 0;
let nextMole = 0;
let effectUntil = 0;
let bannerUntil = 0;
let lastNow = 0;
let mode = "ready";
let endAt = 0;
let secondsLeft = 30;
let hammerStarted = 0;
let hammerPose = -1;
let hammerHole = -1;

for (let i = 0; i < 6; i += 1) {
  const x = 12 + (i % 3) * 103;
  const y = 42 + Math.floor(i / 3) * 87;
  holes.push({ x, y });
  Pocket.rect(x, y + 11, 88, 64, "#CDE8B8", 32);
  Pocket.rect(x + 8, y + 23, 72, 49, "#A8D396", 25);
  const earL = Pocket.rect(x + 16, y + 5, 20, 24, "#A86B43", 10);
  const earR = Pocket.rect(x + 52, y + 5, 20, 24, "#A86B43", 10);
  const body = Pocket.rect(x + 13, y + 12, 62, 57, "#A86B43", 29);
  const innerL = Pocket.rect(x + 22, y + 9, 8, 12, "#F7B9B2", 5);
  const innerR = Pocket.rect(x + 58, y + 9, 8, 12, "#F7B9B2", 5);
  const eyeL = Pocket.rect(x + 30, y + 32, 5, 7, "#2E2420", 3);
  const eyeR = Pocket.rect(x + 53, y + 32, 5, 7, "#2E2420", 3);
  const cheekL = Pocket.rect(x + 21, y + 42, 11, 7, "#F4A29B", 4);
  const cheekR = Pocket.rect(x + 56, y + 42, 11, 7, "#F4A29B", 4);
  const nose = Pocket.rect(x + 40, y + 40, 8, 6, "#F9C4BF", 3);
  const smileL = Pocket.rect(x + 34, y + 49, 4, 3, "#744038", 2);
  const smile = Pocket.rect(x + 38, y + 52, 12, 3, "#744038", 2);
  const smileR = Pocket.rect(x + 50, y + 49, 4, 3, "#744038", 2);
  moles.push([earL, earR, body, innerL, innerR, eyeL, eyeR, cheekL, cheekR, nose, smileL, smile, smileR]);
  for (const part of moles[i]) Pocket.setVisible(part, false);
  Pocket.pressArea(x, y, 88, 76, () => touchHole(i));
}

for (let i = 0; i < 8; i += 1) {
  sparks.push(Pocket.rect(0, 0, 11, 11, "#FFD84D", 6));
  Pocket.setVisible(sparks[i], false);
}

const reactionParts = [
  [0, 0, 40, 40, "#FFE382", 20],
  [11, 13, 4, 5, "#3D2C25", 2], [25, 13, 4, 5, "#3D2C25", 2],
  [6, 22, 7, 5, "#F6A8AA", 3], [27, 22, 7, 5, "#F6A8AA", 3],
  [13, 25, 3, 3, "#A34D54", 2], [16, 28, 8, 3, "#A34D54", 2], [24, 25, 3, 3, "#A34D54", 2]
];
for (const part of reactionParts) {
  const handle = Pocket.rect(part[0], part[1], part[2], part[3], part[4], part[5]);
  Pocket.setVisible(handle, false);
  reaction.push(handle);
}

const roundPanel = [
  Pocket.rect(38, 52, 246, 149, "#D7DEC7", 20),
  Pocket.rect(34, 48, 250, 149, "#FFF9E9", 20),
  Pocket.rect(139, 54, 42, 42, "#FFD872", 21),
  Pocket.rect(150, 68, 4, 6, "#513923", 2),
  Pocket.rect(166, 68, 4, 6, "#513923", 2),
  Pocket.rect(150, 81, 3, 3, "#E67170", 2),
  Pocket.rect(153, 84, 14, 3, "#E67170", 2),
  Pocket.rect(167, 81, 3, 3, "#E67170", 2)
];
const roundTitle = Pocket.text("READY?", 75, 99, 170, "#5C3726");
const roundNumber = Pocket.number(112, 114, 96, 40, "#5C3726");
Pocket.setText(roundNumber, "30");
const replayButton = Pocket.rect(92, 157, 136, 31, "#FFB45F", 15);
const replayLabel = Pocket.text("START", 109, 164, 102, "#5C3726");
Pocket.setTextAlign(roundTitle, "center");
Pocket.setTextAlign(replayLabel, "center");
roundPanel.push(roundTitle, roundNumber, replayButton, replayLabel);
const replayArea = Pocket.pressArea(92, 157, 136, 31, startRound);

const hammerOffsets = [
  [[54, -7, 9, 36, "#B87950", 4], [41, -10, 36, 16, "#A9B9D5", 7], [45, -7, 27, 4, "#ECF2FA", 2]],
  [[43, 1, 9, 36, "#B87950", 4], [24, 27, 47, 17, "#A9B9D5", 8], [29, 30, 37, 4, "#ECF2FA", 2]],
  [[30, -5, 9, 36, "#B87950", 4], [17, -8, 36, 16, "#A9B9D5", 7], [21, -5, 27, 4, "#ECF2FA", 2]]
];
const hammerFrames = hammerOffsets.map(parts => parts.map(part => {
  const handle = Pocket.rect(0, 0, part[2], part[3], part[4], part[5]);
  Pocket.setVisible(handle, false);
  return handle;
}));

function setHammerPose(pose) {
  if (hammerPose === pose) return;
  for (const frame of hammerFrames) for (const part of frame) Pocket.setVisible(part, false);
  hammerPose = pose;
  if (pose < 0) return;
  const hole = holes[hammerHole];
  for (let i = 0; i < hammerFrames[pose].length; i += 1) {
    Pocket.setPosition(hammerFrames[pose][i], hole.x + hammerOffsets[pose][i][0],
                       hole.y + hammerOffsets[pose][i][1]);
    Pocket.setVisible(hammerFrames[pose][i], true);
  }
}

function showHammer(index) {
  hammerHole = index;
  hammerStarted = lastNow;
  hammerPose = -1;
  setHammerPose(0);
}

function showRoundPanel(visible) {
  for (const part of roundPanel) Pocket.setVisible(part, visible);
  Pocket.setVisible(replayArea, visible);
}

function startRound() {
  mode = "running";
  endAt = lastNow + 30000;
  secondsLeft = 30;
  hits = points = spawns = 0;
  nextMole = lastNow + 1700;
  effectUntil = bannerUntil = 0;
  Pocket.setText(score, "0");
  Pocket.setText(timer, "0:30");
  Pocket.setText(banner, "MOLE POP");
  Pocket.setColor(mascot, "#FFD872");
  Pocket.setSize(timeFill, 210, 8);
  for (const light of progress) Pocket.setColor(light, "#E5C79F");
  for (const spark of sparks) Pocket.setVisible(spark, false);
  for (const part of reaction) Pocket.setVisible(part, false);
  setHammerPose(-1);
  showRoundPanel(false);
  showMole(Math.floor(Math.random() * moles.length));
}

function finishRound() {
  mode = "over";
  showMole(-1);
  Pocket.setText(timer, "0:00");
  Pocket.setSize(timeFill, 1, 8);
  Pocket.setText(banner, "YAY!");
  Pocket.setText(roundTitle, "SCORE");
  Pocket.setText(roundNumber, String(points));
  Pocket.setText(replayLabel, "NEW GAME");
  for (const spark of sparks) Pocket.setVisible(spark, false);
  for (const part of reaction) Pocket.setVisible(part, false);
  setHammerPose(-1);
  effectUntil = bannerUntil = 0;
  showRoundPanel(true);
  Pocket.tone(880, 65);
  Pocket.tone(1100, 85);
}

function showMole(index) {
  if (active >= 0) for (const part of moles[active]) Pocket.setVisible(part, false);
  active = index;
  if (active < 0) return;
  spawns += 1;
  golden = spawns % 4 === 0;
  for (const part of [0, 1, 2])
    Pocket.setColor(moles[active][part], golden ? "#FFC94F" : "#A86B43");
  for (const part of moles[active]) Pocket.setVisible(part, true);
}

function showSparks(index, color) {
  const hole = holes[index];
  const offsets = [[-28,-17],[-5,-29],[18,-22],[31,0],[21,24],[-3,31],[-26,23],[-36,3]];
  for (let i = 0; i < sparks.length; i += 1) {
    Pocket.setPosition(sparks[i], hole.x + 38 + offsets[i][0], hole.y + 34 + offsets[i][1]);
    Pocket.setColor(sparks[i], color);
    Pocket.setVisible(sparks[i], true);
  }
  for (let i = 0; i < reaction.length; i += 1) {
    Pocket.setPosition(reaction[i], hole.x + 24 + reactionParts[i][0], hole.y + 19 + reactionParts[i][1]);
    Pocket.setVisible(reaction[i], true);
  }
  Pocket.setColor(reaction[0], color === "#90CDF2" ? "#B8E7FA" : "#FFE382");
  effectUntil = lastNow + 600;
}

function touchHole(index) {
  if (mode !== "running") return;
  showHammer(index);
  if (index !== active) {
    showSparks(index, "#90CDF2");
    Pocket.setColor(mascot, "#B8E7FA");
    Pocket.setText(banner, "BOOP!");
    bannerUntil = lastNow + 500;
    return;
  }
  const bonus = golden ? 3 : 1;
  points += bonus;
  hits += 1;
  Pocket.setText(score, String(points));
  Pocket.setColor(progress[(hits - 1) % 5], "#FF9F43");
  Pocket.setColor(mascot, golden ? "#FFC94F" : "#FFABC8");
  showSparks(index, golden ? "#FFBF26" : "#FF765A");
  showMole(-1);
  nextMole = lastNow + 650;
  if (hits % 5 === 0) {
    points += 5;
    Pocket.setText(score, String(points));
    Pocket.setText(banner, "SUPER! +5");
    for (const light of progress) Pocket.setColor(light, "#E5C79F");
    Pocket.tone(950, 65);
    Pocket.tone(1200, 85);
  } else {
    Pocket.setText(banner, golden ? "GOLD! +3" : "NICE! +1");
    Pocket.tone(golden ? 1050 : 760, golden ? 65 : 35);
  }
  bannerUntil = lastNow + 900;
}

function frame(now) {
  lastNow = now;
  if (mode !== "running") return;
  const millisecondsLeft = endAt - now;
  if (millisecondsLeft <= 0) { finishRound(); return; }
  const remaining = Math.ceil(millisecondsLeft / 1000);
  if (remaining !== secondsLeft) {
    secondsLeft = remaining;
    Pocket.setText(timer, Math.floor(remaining / 60) + ":" + String(remaining % 60).padStart(2, "0"));
    Pocket.setSize(timeFill, Math.max(1, Math.ceil(210 * remaining / 30)), 8);
  }
  if (hammerPose >= 0) {
    const elapsed = now - hammerStarted;
    setHammerPose(elapsed >= 420 ? -1 : elapsed < 120 ? 0 : elapsed < 260 ? 1 : 2);
  }
  if (effectUntil && now >= effectUntil) {
    for (const spark of sparks) Pocket.setVisible(spark, false);
    for (const part of reaction) Pocket.setVisible(part, false);
    effectUntil = 0;
  }
  if (bannerUntil && now >= bannerUntil) {
    Pocket.setText(banner, "MOLE POP");
    Pocket.setColor(mascot, "#FFD872");
    bannerUntil = 0;
  }
  if (now < nextMole) return;
  const previous = active;
  let next = Math.floor(Math.random() * moles.length);
  if (next === previous) next = (next + 1) % moles.length;
  showMole(next);
  nextMole = now + 1700;
}
