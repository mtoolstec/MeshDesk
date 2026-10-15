const assert = require("assert");
const fs = require("fs");
const vm = require("vm");

function load(id) {
  const widgets = [];
  const presses = [];
  const tones = [];
  let fullscreen = false;
  const make = (type, text, x, y, width, height, color) => {
    widgets.push({ type, text, x, y, width, height, color, visible: true });
    return widgets.length - 1;
  };
  const Pocket = {
    setBackground() {},
    setFullscreen(value) { fullscreen = value; },
    rect: (x, y, width, height, color) => make("rect", "", x, y, width, height, color),
    text: (text, x, y, width, color) => make("text", text, x, y, width, 14, color),
    number: (x, y, width, height, color) => make("number", "", x, y, width, height, color),
    pressArea(x, y, width, height, callback) {
      presses.push(callback);
      return make("press", "", x, y, width, height, "");
    },
    setVisible: (handle, visible) => { widgets[handle].visible = visible; },
    setText: (handle, text) => { widgets[handle].text = text; },
    setTextAlign: (handle, align) => { widgets[handle].align = align; },
    setColor: (handle, color) => { widgets[handle].color = color; },
    setPosition: (handle, x, y) => { Object.assign(widgets[handle], { x, y }); },
    setSize: (handle, width, height) => { Object.assign(widgets[handle], { width, height }); },
    tone(frequency, duration) { tones.push([frequency, duration]); }
  };
  const source = fs.readFileSync(`apps/${id}/app.js`, "utf8");
  const context = vm.createContext({ Pocket, Math });
  vm.runInContext(source, context);
  return {
    widgets, presses, tones, frame: now => vm.runInContext(`frame(${now})`, context),
    state: expression => vm.runInContext(expression, context),
    fullscreen: () => fullscreen
  };
}

const mole = load("whack-a-mole");
assert(mole.fullscreen(), "mole game should claim the full display");
mole.frame(0);
const moleScore = mole.widgets.find(widget => widget.type === "text" && widget.x === 16);
const moleBanner = mole.widgets.find(widget => widget.type === "text" && widget.x === 107);
const moleTimer = mole.widgets.find(widget => widget.type === "text" && widget.x === 22);
assert.equal(mole.state("mode"), "ready");
assert.equal(moleTimer.text, "0:30");
mole.presses[6]();
assert.equal(mole.state("mode"), "running");
assert.equal(moleScore.text, "0", "starting a round should reset the score");
const firstMole = mole.state("active");
mole.presses[(firstMole + 1) % 6]();
assert.equal(moleScore.text, "0", "a missed tap should give feedback without losing points");
assert.equal(moleBanner.text, "BOOP!");
assert(mole.widgets[mole.state("reaction[0]")].visible, "every tap should show a smiling face");
mole.presses[firstMole]();
assert.equal(moleScore.text, "1");
assert.equal(mole.state("hammerPose"), 0, "a tap should raise the hammer");
mole.frame(150);
assert.equal(mole.state("hammerPose"), 1, "the hammer should strike");
mole.frame(300);
assert.equal(mole.state("hammerPose"), 2, "the hammer should rebound");
mole.frame(450);
assert.equal(mole.state("hammerPose"), -1, "the hammer should clear after the animation");
for (let i = 1; i < 5; i += 1) {
  mole.frame(i * 750);
  mole.presses[mole.state("active")]();
}
assert.equal(moleScore.text, "12", "golden mole and five-hit celebration should award bonus points");
assert.equal(moleBanner.text, "SUPER! +5");
mole.frame(3700);
assert.equal(mole.widgets[mole.state("reaction[0]")].visible, false, "reaction face should clear after its animation");
mole.frame(29000);
assert.equal(moleTimer.text, "0:01");
mole.frame(30000);
assert.equal(mole.state("mode"), "over");
assert.equal(moleTimer.text, "0:00");
assert.equal(mole.state("active"), -1);
assert.equal(mole.widgets[mole.state("roundPanel[0]")].visible, true);
mole.frame(31000);
assert.equal(mole.state("active"), -1, "moles must stop spawning after time expires");
mole.presses[0]();
assert.equal(mole.state("mode"), "over", "only the new game button should restart a round");
assert.equal(mole.widgets[mole.state("roundNumber")].text, "12", "the final score should appear in the floating panel");
assert.equal(mole.widgets[mole.state("replayLabel")].text, "NEW GAME");
mole.presses[6]();
assert.equal(mole.state("mode"), "running");
assert.equal(moleScore.text, "0", "replaying starts a fresh thirty-second round");
assert.equal(moleTimer.text, "0:30");

const bubbles = load("bubble-pop");
assert(bubbles.fullscreen(), "bubble game should claim the full display");
bubbles.frame(0);
assert.equal(bubbles.state("mode"), "ready");
bubbles.presses[7]();
assert.equal(bubbles.state("mode"), "running");
const startToneCount = bubbles.tones.length;
bubbles.presses[1]();
assert.equal(bubbles.state("points"), 1);
assert.equal(bubbles.tones.length, startToneCount + 1, "a normal bubble should make a short pop sound");
assert.equal(bubbles.widgets[bubbles.state("bubbles[0].parts[1]")].visible, false);
assert.equal(bubbles.widgets[bubbles.state("burst[0]")].visible, true);
bubbles.presses[1]();
assert.equal(bubbles.state("points"), 1, "a popped bubble cannot score twice before respawning");
bubbles.presses[6]();
assert.equal(bubbles.state("points"), 6, "the star bubble should award five points");
assert.equal(bubbles.tones.length, startToneCount + 3, "a star bubble should play a different two-note reward");
assert.equal(bubbles.state("starIndex"), -1);
bubbles.presses[2]();
bubbles.presses[3]();
assert.equal(bubbles.state("combo"), 4);
assert(bubbles.state("starIndex") >= 0, "a new star bubble should appear after two more pops");
bubbles.frame(150);
assert.equal(bubbles.state("burstPhase"), 1, "confetti should spread after a tap");
bubbles.frame(300);
assert.equal(bubbles.state("burstPhase"), 2);
bubbles.frame(500);
assert.equal(bubbles.widgets[bubbles.state("burst[0]")].visible, false);
assert.equal(bubbles.widgets[bubbles.state("bubbles[0].parts[1]")].visible, true);
for (let i = 1; i <= 6; i += 1) bubbles.presses[i]();
assert.equal(bubbles.state("pops"), 10);
assert(bubbles.state("partyUntil") > 500, "ten pops should start rainbow bonus time");
assert.equal(bubbles.widgets[bubbles.state("celebration[0]")].visible, true);
assert.equal(bubbles.widgets[bubbles.state("rewardBadges[0]")].color, "#FFD36D");
const scoreBeforeMute = bubbles.state("points");
const tonesBeforeMute = bubbles.tones.length;
bubbles.presses[0]();
bubbles.frame(1000);
bubbles.presses[1]();
assert(bubbles.state("points") > scoreBeforeMute, "rainbow time should keep rewarding pops");
assert.equal(bubbles.tones.length, tonesBeforeMute, "sound toggle should mute game sounds");
bubbles.frame(5000);
assert.equal(bubbles.state("partyUntil"), 0);
assert.equal(bubbles.widgets[bubbles.state("celebration[0]")].visible, false);
bubbles.frame(29000);
assert.equal(bubbles.widgets[bubbles.state("timer")].text, "0:01");
bubbles.frame(30000);
assert.equal(bubbles.state("mode"), "over");
assert.equal(bubbles.widgets[bubbles.state("panelNumber")].text, String(bubbles.state("points")));
assert.equal(bubbles.widgets[bubbles.state("panelButtonLabel")].text, "NEW GAME");
bubbles.presses[1]();
assert.equal(bubbles.state("mode"), "over");
bubbles.presses[7]();
assert.equal(bubbles.state("mode"), "running");
assert.equal(bubbles.state("points"), 0);
assert.equal(bubbles.widgets[bubbles.state("rewardBadges[0]")].color, "#C9DEC4");

const piano = load("little-piano");
assert(piano.fullscreen());
assert.equal(piano.state("mode"), "guide");
for (const key of [0, 1, 2, 1, 0]) piano.presses[key]();
assert.equal(piano.state("stars"), 1, "finishing a guided tune should award a star");
assert.equal(piano.widgets[piano.state("celebration[0]")].visible, true);
piano.frame(1200);
assert.equal(piano.widgets[piano.state("celebration[0]")].visible, false);
piano.presses[6]();
assert.equal(piano.state("mode"), "free");
for (let i = 0; i < 8; i += 1) piano.presses[i % 5]();
assert.equal(piano.state("stars"), 2, "free play should also reward eight notes");
piano.presses[5]();
const pianoToneCount = piano.tones.length;
piano.presses[0]();
assert.equal(piano.tones.length, pianoToneCount, "piano sound switch should mute notes");

const fruit = load("fruit-party");
assert(fruit.fullscreen());
fruit.frame(0);
fruit.presses[3]();
assert.equal(fruit.state("mode"), "running");
fruit.presses[0]();
assert.equal(fruit.state("points"), 1);
fruit.presses[0]();
assert.equal(fruit.state("points"), 1, "sliced fruit should not score twice");
fruit.presses[2]();
assert.equal(fruit.state("points"), 5, "gold fruit should award four points");
fruit.frame(120);
assert.equal(fruit.state("slicePhase"), 1);
fruit.frame(240);
assert.equal(fruit.state("slicePhase"), 2);
fruit.frame(500);
assert.equal(fruit.state("fruits[0].wakeAt"), 0);
for (const index of [0, 1, 2]) fruit.presses[index]();
assert.equal(fruit.state("hits"), 5);
assert.equal(fruit.widgets[fruit.state("baskets[0]")].color, "#FFCA67");
fruit.frame(29000);
assert.equal(fruit.widgets[fruit.state("timer")].text, "0:01");
fruit.frame(30000);
assert.equal(fruit.state("mode"), "over");
assert.equal(fruit.widgets[fruit.state("panelNumber")].text, String(fruit.state("points")));
fruit.presses[0]();
assert.equal(fruit.state("mode"), "over");
fruit.presses[3]();
assert.equal(fruit.state("points"), 0);

const puzzle = load("animal-puzzle");
assert(puzzle.fullscreen());
assert.equal(puzzle.state("mode"), "playing");
puzzle.presses[0]();
assert.equal(puzzle.state("selected"), 0);
puzzle.presses[0]();
assert.equal(puzzle.state("selected"), -1);
for (const [first, second] of [[0, 1], [1, 3], [2, 3]]) {
  puzzle.presses[first]();
  puzzle.presses[second]();
}
assert.equal(puzzle.state("mode"), "solved");
assert.equal(puzzle.state("solved"), 1);
assert.equal(puzzle.widgets[puzzle.state("badges[0]")].color, "#FFD369");
assert.equal(puzzle.widgets[puzzle.state("panel[0]")].visible, true);
puzzle.presses[4]();
assert.equal(puzzle.state("mode"), "playing");
assert.equal(puzzle.state("themeIndex"), 1);

const colors = load("color-match");
colors.presses[0]();
assert.equal(colors.widgets.find(widget => widget.type === "text" && widget.x === 18).text, "1");

const pairs = load("memory-pairs");
const labels = pairs.widgets.filter(widget => widget.type === "text" && (widget.text === "1" || widget.text === "2"));
assert.equal(labels.length, 4);
const matching = labels.map((label, index) => ({ label, index })).filter(item => item.label.text === labels[0].text);
pairs.presses[matching[0].index]();
pairs.presses[matching[1].index]();
assert.equal(labels[matching[0].index].visible, true);
console.log("kids games interaction tests passed");
