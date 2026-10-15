Pocket.setBackground("#F8FBFF");
Pocket.text("Color Match", 105, 3, 180, "#2D4059");
const colors = ["#FF625D", "#4DBB79", "#4A8FE7"];
const choices = [];
const token = Pocket.rect(122, 39, 76, 76, colors[0], 38);
const score = Pocket.text("0", 18, 4, 50, "#2D4059");
let target = 0;
let matches = 0;

for (let i = 0; i < 3; i += 1) {
  const x = 17 + i * 102;
  Pocket.rect(x, 135, 82, 63, colors[i], 16);
  choices.push(Pocket.pressArea(x, 131, 86, 72, () => {
    if (i !== target) return;
    matches += 1;
    Pocket.setText(score, String(matches));
    Pocket.tone(880, 35);
    const previous = target;
    target = Math.floor(Math.random() * 3);
    if (target === previous) target = (target + 1) % 3;
    Pocket.setColor(token, colors[target]);
  }));
}
