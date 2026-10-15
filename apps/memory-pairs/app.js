Pocket.setBackground("#F1EEFF");
Pocket.text("Pair Cards", 112, 4, 170, "#41377D");
const message = Pocket.text("", 22, 190, 280, "#41377D");
const cards = [];
const symbols = ["1", "1", "2", "2"];
const colors = ["#FFB84D", "#FFB84D", "#70C8B0", "#70C8B0"];
let first = -1;
let second = -1;
let hideAt = 0;
let pairs = 0;

shuffle();

for (let i = 0; i < 4; i += 1) {
  const x = 18 + (i % 2) * 153;
  const y = 32 + Math.floor(i / 2) * 78;
  const back = Pocket.rect(x, y, 133, 66, "#6B5CE7", 14);
  const face = Pocket.rect(x, y, 133, 66, symbols[i] === "1" ? colors[0] : colors[2], 14);
  const label = Pocket.text(symbols[i], x + 58, y + 23, 30, "#352B4A");
  Pocket.setVisible(face, false);
  Pocket.setVisible(label, false);
  cards.push({ back, face, label, open: false, matched: false });
  Pocket.pressArea(x, y, 133, 66, () => turn(i));
}

function turn(i) {
  if (pairs === 2) { restart(); return; }
  const card = cards[i];
  if (hideAt || card.open || card.matched) return;
  card.open = true;
  Pocket.setVisible(card.back, false);
  Pocket.setVisible(card.face, true);
  Pocket.setVisible(card.label, true);
  if (first < 0) { first = i; return; }
  second = i;
  if (symbols[first] === symbols[second]) {
    cards[first].matched = cards[second].matched = true;
    first = second = -1;
    pairs += 1;
    Pocket.tone(850, 35);
    if (pairs === 2) Pocket.setText(message, "Great! Tap to play again");
  } else {
    hideAt = -1;
  }
}

function shuffle() {
  for (let i = symbols.length - 1; i > 0; i -= 1) {
    const j = Math.floor(Math.random() * (i + 1));
    [symbols[i], symbols[j]] = [symbols[j], symbols[i]];
  }
}

function restart() {
  shuffle();
  first = second = -1;
  hideAt = pairs = 0;
  Pocket.setText(message, "");
  for (let i = 0; i < cards.length; i += 1) {
    const card = cards[i];
    card.open = card.matched = false;
    Pocket.setText(card.label, symbols[i]);
    Pocket.setColor(card.face, symbols[i] === "1" ? colors[0] : colors[2]);
    Pocket.setVisible(card.back, true);
    Pocket.setVisible(card.face, false);
    Pocket.setVisible(card.label, false);
  }
}

function frame(now) {
  if (hideAt === -1) { hideAt = now + 850; return; }
  if (!hideAt || now < hideAt) return;
  for (const i of [first, second]) {
    cards[i].open = false;
    Pocket.setVisible(cards[i].back, true);
    Pocket.setVisible(cards[i].face, false);
    Pocket.setVisible(cards[i].label, false);
  }
  first = second = -1;
  hideAt = 0;
}
