Pocket.setFullscreen(true);

const GLYPHS = "01ABCDEF#$%<>/\\{}[]";
const COLS = 20, STEP = 16, BOTTOM = 230;
Pocket.setBackground("#000000");

function glyph() { return GLYPHS[Math.floor(Math.random() * GLYPHS.length)]; }
function columnText(length) {
  let value = "";
  for (let i = 0; i < length; i += 1) value += glyph() + (i + 1 < length ? "\n" : "");
  return value;
}

const streams = [];
for (let i = 0; i < COLS; i += 1) {
  const length = 5 + Math.floor(Math.random() * 7);
  const y = -Math.floor(Math.random() * 260);
  streams.push({
    x: 2 + i * STEP,
    y,
    speed: 65 + Math.floor(Math.random() * 70),
    length,
    nextGlyph: 0,
    trail: Pocket.text(columnText(length), 2 + i * STEP, y, 14, "#00A846"),
    head: Pocket.text(glyph(), 2 + i * STEP, y + (length - 1) * 14, 14, "#B8FFD0")
  });
}

let lastFrame = 0;
function frame(now) {
  if (now - lastFrame < 80) return;
  const dt = Math.min(120, now - lastFrame || 80) / 1000;
  lastFrame = now;
  for (let i = 0; i < streams.length; i += 1) {
    const s = streams[i];
    s.y += s.speed * dt;
    if (s.y > BOTTOM + 20) {
      s.y = -s.length * 14 - Math.floor(Math.random() * 100);
      s.speed = 65 + Math.floor(Math.random() * 70);
    }
    const y = Math.floor(s.y), headY = y + (s.length - 1) * 14;
    Pocket.setPosition(s.trail, s.x, y);
    Pocket.setPosition(s.head, s.x, headY);
    if (now >= s.nextGlyph) {
      s.nextGlyph = now + 260 + Math.floor(Math.random() * 300);
      Pocket.setText(s.trail, columnText(s.length));
      Pocket.setText(s.head, glyph());
    }
  }
}
