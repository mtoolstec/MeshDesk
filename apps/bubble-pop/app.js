Pocket.setFullscreen(true);
Pocket.setBackground("#EAF8FF");
Pocket.rect(0, 0, 320, 43, "#FFF9E9", 0);
Pocket.rect(0, 207, 320, 33, "#DDF1D4", 0);
Pocket.rect(9, 211, 34, 22, "#C6E9BC", 15);
Pocket.rect(275, 214, 35, 20, "#C6E9BC", 15);
Pocket.rect(46, 213, 7, 7, "#FFB8CC", 4);
Pocket.rect(268, 211, 8, 8, "#FFD47C", 4);

const score = Pocket.text("0", 18, 10, 50, "#5C3D54");
const title = Pocket.text("BUBBLE POP", 83, 9, 155, "#5C3D54");
Pocket.setTextAlign(title, "center");
const timer = Pocket.text("0:30", 262, 10, 50, "#5C3D54");
Pocket.rect(20, 35, 280, 5, "#D8E9D0", 3);
const timeFill = Pocket.rect(20, 35, 280, 5, "#FFBE83", 3);
const rewardBadges = [];
for (let i = 0; i < 3; i += 1) {
  rewardBadges.push(Pocket.rect(15 + i * 25, 213, 19, 19, "#C9DEC4", 10));
  Pocket.rect(22 + i * 25, 219, 6, 6, "#FFFFFF", 3);
}
const soundButton = Pocket.rect(264, 211, 45, 24, "#FFF9E9", 10);
const soundLabel = Pocket.text("SND", 269, 215, 35, "#5C3D54");
Pocket.setTextAlign(soundLabel, "center");

const colors = ["#FFB9D3", "#A8DAFF", "#FFE294", "#B8E7C6", "#D4C0F8", "#FFC3AF"];
const partyColors = ["#FF8CAB", "#79C6FF", "#FFD365", "#8CDEA5", "#BDA0F5", "#FFAA82"];
const bubbles = [];
let mode = "ready";
let lastNow = 0;
let endAt = 0;
let secondsLeft = 30;
let points = 0;
let pops = 0;
let burstUntil = 0;
let bannerUntil = 0;
let burstIndex = -1;
let burstStarted = 0;
let burstPhase = -1;
let lastPopAt = -10000;
let combo = 0;
let starIndex = -1;
let nextStarAt = 0;
let partyUntil = 0;
let celebrationUntil = 0;
let soundOn = true;

function playNotes(notes) {
  if (!soundOn) return;
  for (const note of notes) Pocket.tone(note[0], note[1]);
}

Pocket.pressArea(264, 211, 45, 24, () => {
  soundOn = !soundOn;
  Pocket.setText(soundLabel, soundOn ? "SND" : "OFF");
  Pocket.setColor(soundButton, soundOn ? "#FFF9E9" : "#D1DCE1");
  if (soundOn) playNotes([[880, 45]]);
});

for (let i = 0; i < 6; i += 1) {
  const x = 18 + (i % 3) * 106;
  const y = 49 + Math.floor(i / 3) * 80;
  const parts = [
    Pocket.rect(x - 3, y - 3, 76, 76, "#FFFFFF", 38),
    Pocket.rect(x, y, 70, 70, colors[i], 35),
    Pocket.rect(x + 12, y + 10, 18, 8, "#FFFFFF", 5),
    Pocket.rect(x + 21, y + 31, 4, 6, "#65465A", 2),
    Pocket.rect(x + 45, y + 31, 4, 6, "#65465A", 2),
    Pocket.rect(x + 14, y + 42, 9, 6, "#F59EAD", 3),
    Pocket.rect(x + 47, y + 42, 9, 6, "#F59EAD", 3),
    Pocket.rect(x + 27, y + 46, 3, 3, "#9B5E70", 2),
    Pocket.rect(x + 30, y + 49, 10, 3, "#9B5E70", 2),
    Pocket.rect(x + 40, y + 46, 3, 3, "#9B5E70", 2)
  ];
  const starMarks = [
    Pocket.rect(x + 23, y + 15, 24, 13, "#FFF4B5", 7),
    Pocket.rect(x + 29, y + 11, 12, 6, "#FFF4B5", 3),
    Pocket.rect(x + 32, y + 17, 6, 7, "#FFAC56", 3)
  ];
  for (const part of starMarks) Pocket.setVisible(part, false);
  bubbles.push({ x, y, parts, starMarks, wakeAt: 0 });
  Pocket.pressArea(x - 4, y - 4, 78, 78, () => popBubble(i));
}

const burst = [
  Pocket.rect(0, 0, 42, 42, "#FFE48A", 21),
  Pocket.rect(0, 0, 4, 5, "#65465A", 2),
  Pocket.rect(0, 0, 4, 5, "#65465A", 2),
  Pocket.rect(0, 0, 3, 3, "#A65F71", 2),
  Pocket.rect(0, 0, 10, 3, "#A65F71", 2),
  Pocket.rect(0, 0, 3, 3, "#A65F71", 2)
];
const confetti = [];
for (let i = 0; i < 8; i += 1)
  confetti.push(Pocket.rect(0, 0, 9, 9, colors[i % colors.length], 5));
const confettiOffsets = [[-8, 4], [24, -13], [68, -4], [80, 24], [68, 66], [30, 77], [-8, 64], [-20, 30]];

function spreadConfetti(phase) {
  if (burstIndex < 0 || phase === burstPhase) return;
  burstPhase = phase;
  const bubble = bubbles[burstIndex];
  const scale = phase === 0 ? 0.3 : phase === 1 ? 0.7 : 1;
  for (let i = 0; i < confetti.length; i += 1) {
    Pocket.setPosition(confetti[i], bubble.x + Math.round(35 + (confettiOffsets[i][0] - 35) * scale),
                       bubble.y + Math.round(35 + (confettiOffsets[i][1] - 35) * scale));
  }
}

function showBurst(index, special) {
  const bubble = bubbles[index];
  const face = [[14, 14], [25, 27], [39, 27], [27, 39], [30, 42], [40, 39]];
  Pocket.setColor(burst[0], special ? "#FFD36D" : "#FFE48A");
  for (let i = 0; i < burst.length; i += 1) {
    Pocket.setPosition(burst[i], bubble.x + face[i][0], bubble.y + face[i][1]);
    Pocket.setVisible(burst[i], true);
  }
  for (const part of confetti) Pocket.setVisible(part, true);
  burstIndex = index;
  burstStarted = lastNow;
  burstPhase = -1;
  spreadConfetti(0);
  burstUntil = lastNow + 420;
}

function hideBurst() {
  for (const part of burst) Pocket.setVisible(part, false);
  for (const part of confetti) Pocket.setVisible(part, false);
  burstUntil = 0;
  burstIndex = -1;
  burstPhase = -1;
}
hideBurst();

const panel = [
  Pocket.rect(35, 49, 250, 157, "#D0DFD7", 20),
  Pocket.rect(31, 45, 254, 157, "#FFF9E9", 20),
  Pocket.rect(140, 51, 40, 40, "#FFCF83", 20),
  Pocket.rect(150, 65, 4, 5, "#65465A", 2),
  Pocket.rect(166, 65, 4, 5, "#65465A", 2),
  Pocket.rect(151, 79, 3, 3, "#A65F71", 2),
  Pocket.rect(154, 82, 12, 3, "#A65F71", 2),
  Pocket.rect(166, 79, 3, 3, "#A65F71", 2)
];
const panelTitle = Pocket.text("POP THE BUBBLES!", 54, 96, 212, "#5C3D54");
const panelNumber = Pocket.number(112, 109, 96, 40, "#5C3D54");
Pocket.setText(panelNumber, "30");
const panelReward = Pocket.text("HAPPY POPPING!", 54, 150, 212, "#A66B7A");
const panelButton = Pocket.rect(91, 169, 138, 30, "#FFB76B", 15);
const panelButtonLabel = Pocket.text("START", 108, 176, 104, "#5C3D54");
Pocket.setTextAlign(panelTitle, "center");
Pocket.setTextAlign(panelReward, "center");
Pocket.setTextAlign(panelButtonLabel, "center");
panel.push(panelTitle, panelNumber, panelReward, panelButton, panelButtonLabel);
const startArea = Pocket.pressArea(91, 169, 138, 30, startRound);

const celebration = [
  Pocket.rect(63, 88, 194, 61, "#FFF5C2", 20),
  Pocket.rect(73, 95, 25, 25, "#FFB9D3", 13),
  Pocket.rect(222, 95, 25, 25, "#A8DAFF", 13)
];
const celebrationTitle = Pocket.text("RAINBOW!", 99, 99, 122, "#77529A");
const celebrationInfo = Pocket.text("DOUBLE POINTS", 78, 124, 164, "#9A6672");
Pocket.setTextAlign(celebrationTitle, "center");
Pocket.setTextAlign(celebrationInfo, "center");
celebration.push(celebrationTitle, celebrationInfo);
for (const part of celebration) Pocket.setVisible(part, false);

function showPanel(visible) {
  for (const part of panel) Pocket.setVisible(part, visible);
  Pocket.setVisible(startArea, visible);
}

function showBubble(index, visible) {
  for (const part of bubbles[index].parts) Pocket.setVisible(part, visible);
  for (const part of bubbles[index].starMarks)
    Pocket.setVisible(part, visible && index === starIndex);
}

function paintBubble(index) {
  const special = index === starIndex;
  const party = partyUntil > lastNow;
  Pocket.setColor(bubbles[index].parts[0], special ? "#FFD36D" : party ? "#FFF3D0" : "#FFFFFF");
  Pocket.setColor(bubbles[index].parts[1], special ? "#FFD36D" :
                  (party ? partyColors : colors)[(index + pops) % colors.length]);
  for (const part of bubbles[index].starMarks)
    Pocket.setVisible(part, !bubbles[index].wakeAt && special);
}

function assignStar() {
  if (starIndex >= 0 || pops < nextStarAt) return;
  for (let offset = 0; offset < bubbles.length; offset += 1) {
    const index = (pops + 2 + offset) % bubbles.length;
    if (bubbles[index].wakeAt) continue;
    starIndex = index;
    paintBubble(index);
    return;
  }
}

function showCelebration() {
  celebrationUntil = lastNow + 1100;
  for (const part of celebration) Pocket.setVisible(part, true);
}

function startRound() {
  if (mode === "running") return;
  mode = "running";
  endAt = lastNow + 30000;
  secondsLeft = 30;
  points = pops = 0;
  combo = 0;
  lastPopAt = -10000;
  starIndex = 5;
  nextStarAt = 0;
  partyUntil = celebrationUntil = 0;
  Pocket.setText(score, "0");
  Pocket.setText(timer, "0:30");
  Pocket.setText(title, "BUBBLE POP");
  Pocket.setSize(timeFill, 280, 5);
  hideBurst();
  bannerUntil = 0;
  for (const part of celebration) Pocket.setVisible(part, false);
  for (const badge of rewardBadges) Pocket.setColor(badge, "#C9DEC4");
  for (let i = 0; i < bubbles.length; i += 1) {
    bubbles[i].wakeAt = 0;
    paintBubble(i);
    showBubble(i, true);
  }
  showPanel(false);
  playNotes([[660, 40], [880, 45]]);
}

function endRound() {
  mode = "over";
  Pocket.setText(timer, "0:00");
  Pocket.setSize(timeFill, 1, 5);
  Pocket.setText(title, "YAY!");
  hideBurst();
  partyUntil = celebrationUntil = 0;
  for (const part of celebration) Pocket.setVisible(part, false);
  for (let i = 0; i < bubbles.length; i += 1) showBubble(i, false);
  Pocket.setText(panelTitle, "GREAT JOB!");
  Pocket.setText(panelNumber, String(points));
  Pocket.setText(panelReward, points >= 70 ? "RAINBOW CHAMP!" : points >= 35 ? "SUPER STAR!" : "HAPPY POPPING!");
  Pocket.setText(panelButtonLabel, "NEW GAME");
  showPanel(true);
  playNotes([[784, 65], [988, 65], [1176, 90]]);
}

function popBubble(index) {
  if (mode !== "running" || bubbles[index].wakeAt) return;
  pops += 1;
  combo = lastNow - lastPopAt <= 1200 ? Math.min(combo + 1, 9) : 1;
  lastPopAt = lastNow;
  const special = index === starIndex;
  const milestone = pops % 10 === 0;
  let earned = (special ? 5 : 1) * (partyUntil > lastNow ? 2 : 1);
  if (combo >= 3) earned += combo >= 5 ? 2 : 1;
  if (milestone) earned += 5;
  points += earned;
  Pocket.setText(score, String(points));
  Pocket.setText(title, milestone ? "RAINBOW +" + earned : special ? "STAR +" + earned :
                 combo >= 3 ? "COMBO " + combo + " +" + earned : "POP! +" + earned);
  bannerUntil = lastNow + 620;
  showBubble(index, false);
  bubbles[index].wakeAt = lastNow + 460;
  showBurst(index, special);
  if (special) {
    starIndex = -1;
    nextStarAt = pops + 2;
  }
  if (milestone) {
    partyUntil = lastNow + 4000;
    if (pops <= 30) Pocket.setColor(rewardBadges[pops / 10 - 1], "#FFD36D");
    for (let i = 0; i < bubbles.length; i += 1) paintBubble(i);
    showCelebration();
    playNotes([[660, 55], [880, 55], [1100, 80]]);
  } else if (special) {
    playNotes([[880, 55], [1175, 75]]);
  } else if (combo >= 3) {
    playNotes([[690 + combo * 55, 35], [870 + combo * 45, 45]]);
  } else {
    playNotes([[620 + (index % 3) * 120, 40]]);
  }
  assignStar();
}

function frame(now) {
  lastNow = now;
  if (mode !== "running") return;
  const millisecondsLeft = endAt - now;
  if (millisecondsLeft <= 0) { endRound(); return; }
  const remaining = Math.ceil(millisecondsLeft / 1000);
  if (remaining !== secondsLeft) {
    secondsLeft = remaining;
    Pocket.setText(timer, "0:" + String(remaining).padStart(2, "0"));
    Pocket.setSize(timeFill, Math.max(1, Math.ceil(280 * remaining / 30)), 5);
  }
  if (burstUntil) {
    const elapsed = now - burstStarted;
    if (now >= burstUntil) hideBurst();
    else spreadConfetti(elapsed < 110 ? 0 : elapsed < 230 ? 1 : 2);
  }
  if (celebrationUntil && now >= celebrationUntil) {
    for (const part of celebration) Pocket.setVisible(part, false);
    celebrationUntil = 0;
  }
  if (partyUntil && now >= partyUntil) {
    partyUntil = 0;
    for (let i = 0; i < bubbles.length; i += 1) paintBubble(i);
  }
  if (bannerUntil && now >= bannerUntil) {
    Pocket.setText(title, partyUntil ? "RAINBOW TIME!" : "BUBBLE POP");
    bannerUntil = 0;
  }
  for (let i = 0; i < bubbles.length; i += 1) {
    if (!bubbles[i].wakeAt || now < bubbles[i].wakeAt) continue;
    bubbles[i].wakeAt = 0;
    paintBubble(i);
    showBubble(i, true);
  }
  assignStar();
}
