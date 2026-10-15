const assert = require("assert");
const fs = require("fs");
const vm = require("vm");

const widgets = [];
let jump;
const makeWidget = (x, y, width, height) => {
  widgets.push({ x, y, width, height });
  return widgets.length - 1;
};
const Pocket = {
  setBackground() {},
  pressArea(x, y, width, height, callback) {
    jump = callback;
    return makeWidget(x, y, width, height);
  },
  rect: makeWidget,
  text(text, x, y, width) { return makeWidget(x, y, width, 14); },
  setPosition(handle, x, y) { Object.assign(widgets[handle], { x, y }); },
  setText() {},
  tone() {}
};

vm.runInNewContext(fs.readFileSync("apps/dinosaur/app.js", "utf8"), { Pocket, Math });
assert.equal(typeof jump, "function");
const head = widgets.find(widget => widget.width === 18 && widget.height === 16);
const groundY = head.y;
jump();
assert(head.y < groundY, "first press should move the dinosaur before the next frame");
const airborneY = head.y;
jump();
assert.equal(head.y, airborneY, "a second press in the air should not restart the jump");
console.log("dinosaur press-to-jump test passed");
