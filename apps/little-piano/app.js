Pocket.setFullscreen(true);
Pocket.setBackground("#FFF9EF");
Pocket.rect(0, 0, 320, 42, "#EDE8FF", 0);
Pocket.rect(0, 209, 320, 31, "#DDF1E3", 0);
const score = Pocket.text("0", 17, 10, 38, "#5A4667");
const banner = Pocket.text("FOLLOW THE LIGHT", 55, 10, 180, "#5A4667");
Pocket.setTextAlign(banner, "center");
const modeButton = Pocket.rect(253, 7, 58, 28, "#C7B9FA", 12);
const modeLabel = Pocket.text("FREE", 260, 13, 45, "#49375D");
Pocket.setTextAlign(modeLabel, "center");

const colors = ["#FFB6C8", "#FFD68D", "#F7EB9D", "#AEE3C7", "#BBD5FF"];
const pressedColors = ["#F28CA9", "#F9BD63", "#EBD771", "#83CDA6", "#8EB5F0"];
const notes = [523, 587, 659, 698, 784];
const songs = [[0, 1, 2, 1, 0], [2, 3, 4, 3, 2], [0, 2, 4, 2, 0]];
const keys = [];
const lights = [];
let mode = "guide";
let songIndex = 0;
let step = 0;
let stars = 0;
let freeNotes = 0;
let soundOn = true;
let lastNow = 0;
let noteUntil = 0;
let celebrationUntil = 0;

for (let i = 0; i < 5; i += 1) {
  const x = 8 + i * 62;
  lights.push(Pocket.rect(x + 13, 49, 32, 20, "#DFD8EA", 10));
  Pocket.rect(x + 2, 78, 58, 124, "#D2D2DE", 12);
  const body = Pocket.rect(x, 75, 58, 124, colors[i], 12);
  Pocket.rect(x + 11, 87, 24, 8, "#FFFFFF", 4);
  Pocket.rect(x + 17, 119, 4, 6, "#59475C", 2);
  Pocket.rect(x + 37, 119, 4, 6, "#59475C", 2);
  Pocket.rect(x + 22, 135, 3, 3, "#A36279", 2);
  Pocket.rect(x + 25, 138, 8, 3, "#A36279", 2);
  Pocket.rect(x + 33, 135, 3, 3, "#A36279", 2);
  keys.push({ x, body, releaseAt: 0 });
  Pocket.pressArea(x, 75, 58, 124, () => playKey(i));
}

const noteFace = [
  Pocket.rect(0, 0, 36, 36, "#FFE48D", 18),
  Pocket.rect(0, 0, 4, 5, "#59475C", 2),
  Pocket.rect(0, 0, 4, 5, "#59475C", 2),
  Pocket.rect(0, 0, 10, 3, "#A36279", 2)
];
for (const part of noteFace) Pocket.setVisible(part, false);
const celebration = [
  Pocket.rect(48, 95, 224, 69, "#FFF3C3", 20),
  Pocket.rect(59, 104, 30, 30, "#FFC2D2", 15),
  Pocket.rect(231, 104, 30, 30, "#BFD9FF", 15)
];
const cheer = Pocket.text("SUPER SONG!", 80, 117, 160, "#714E83");
Pocket.setTextAlign(cheer, "center");
celebration.push(cheer);
for (const part of celebration) Pocket.setVisible(part, false);

const soundButton = Pocket.rect(265, 212, 44, 23, "#FFFFFF", 10);
const soundLabel = Pocket.text("SND", 270, 216, 34, "#59475C");
Pocket.setTextAlign(soundLabel, "center");
Pocket.pressArea(265, 212, 44, 23, () => {
  soundOn = !soundOn;
  Pocket.setText(soundLabel, soundOn ? "SND" : "OFF");
  Pocket.setColor(soundButton, soundOn ? "#FFFFFF" : "#CCD8D5");
});
Pocket.pressArea(253, 7, 58, 28, () => {
  mode = mode === "guide" ? "free" : "guide";
  step = 0;
  freeNotes = 0;
  Pocket.setText(modeLabel, mode === "guide" ? "FREE" : "GUIDE");
  Pocket.setText(banner, mode === "guide" ? "FOLLOW THE LIGHT" : "PLAY ANY KEY!");
  refreshLights();
});

function refreshLights() {
  const target = mode === "guide" ? songs[songIndex][step] : -1;
  for (let i = 0; i < lights.length; i += 1)
    Pocket.setColor(lights[i], i === target ? "#FFD36C" : "#DFD8EA");
}
refreshLights();

function showNote(index) {
  const x = keys[index].x + 11;
  const positions = [[x, 42], [x + 10, 54], [x + 23, 54], [x + 13, 67]];
  for (let i = 0; i < noteFace.length; i += 1) {
    Pocket.setPosition(noteFace[i], positions[i][0], positions[i][1]);
    Pocket.setVisible(noteFace[i], true);
  }
  Pocket.setColor(noteFace[0], colors[index]);
  noteUntil = lastNow + 360;
}

function celebrate() {
  stars += 1;
  Pocket.setText(score, String(stars));
  Pocket.setText(cheer, mode === "guide" ? "SUPER SONG!" : "MUSIC STAR!");
  for (const part of celebration) Pocket.setVisible(part, true);
  celebrationUntil = lastNow + 1100;
  if (soundOn) {
    Pocket.tone(880, 65);
    Pocket.tone(1047, 65);
    Pocket.tone(1319, 95);
  }
}

function playKey(index) {
  Pocket.setColor(keys[index].body, pressedColors[index]);
  keys[index].releaseAt = lastNow + 150;
  showNote(index);
  if (soundOn) Pocket.tone(notes[index], 80);
  if (celebrationUntil > lastNow) return;
  if (mode === "free") {
    freeNotes += 1;
    if (freeNotes % 8 === 0) celebrate();
    return;
  }
  if (index !== songs[songIndex][step]) return;
  step += 1;
  if (step === songs[songIndex].length) {
    celebrate();
    songIndex = (songIndex + 1) % songs.length;
    step = 0;
  }
  refreshLights();
}

function frame(now) {
  lastNow = now;
  for (let i = 0; i < keys.length; i += 1) {
    if (keys[i].releaseAt && now >= keys[i].releaseAt) {
      keys[i].releaseAt = 0;
      Pocket.setColor(keys[i].body, colors[i]);
    }
  }
  if (noteUntil && now >= noteUntil) {
    for (const part of noteFace) Pocket.setVisible(part, false);
    noteUntil = 0;
  }
  if (celebrationUntil && now >= celebrationUntil) {
    for (const part of celebration) Pocket.setVisible(part, false);
    celebrationUntil = 0;
  }
}
