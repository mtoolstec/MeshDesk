const dark = Pocket.device().darkMode;
const BG = dark ? "#000000" : "#F4F4F5";
const CARD = dark ? "#1C1C1E" : "#FFFFFF";
const TEXT = dark ? "#FFFFFF" : "#27272A";
const MUTED = dark ? "#98989D" : "#71717A";
Pocket.setBackground(BG);
const listObjects = [];
const editObjects = [];
let files = [];
let activeFile = "";
function keep(group, handle) { group.push(handle); return handle; }
function show(group, visible) { for (let i = 0; i < group.length; i += 1) Pocket.setVisible(group[i], visible); }

keep(listObjects, Pocket.text("My Notes", 14, 10, 180, TEXT));
keep(listObjects, Pocket.button("New", 238, 4, 68, 34, () => openEditor(""), "#D98C00"));
const rowLabels = [];
for (let i = 0; i < 6; i += 1) {
  keep(listObjects, Pocket.rect(10, 46 + i * 27, 300, 24, CARD, 4));
  rowLabels.push(keep(listObjects, Pocket.text("", 18, 48 + i * 27, 268, TEXT)));
  keep(listObjects, Pocket.hitArea(10, 46 + i * 27, 300, 24, () => { if (i < files.length) openEditor(files[i]); }));
}
const nameField = keep(editObjects, Pocket.textArea(10, 4, 210, 34, 48, true));
const editor = keep(editObjects, Pocket.textArea(10, 42, 300, 126, 8192));
const status = keep(editObjects, Pocket.text("", 12, 180, 126, MUTED));
keep(editObjects, Pocket.button("List", 144, 174, 72, 34, showList, "#52525B"));
keep(editObjects, Pocket.button("Save", 226, 174, 80, 34, saveNote, "#D98C00"));

function normalizedName(value) {
  let name = String(value || "").trim().replace(/[^A-Za-z0-9._-]/g, "-");
  if (!name) name = `note-${Date.now()}`;
  if (!name.toLowerCase().endsWith(".txt")) name += ".txt";
  return name.slice(0, 48);
}
function refreshList() {
  files = Pocket.listFiles();
  for (let i = 0; i < rowLabels.length; i += 1)
    Pocket.setText(rowLabels[i], i < files.length ? files[i].replace(/\.txt$/i, "") : "");
}
function showList() { refreshList(); show(editObjects, false); show(listObjects, true); }
function openEditor(file) {
  activeFile = file;
  Pocket.setText(nameField, file ? file.replace(/\.txt$/i, "") : "Untitled");
  Pocket.setText(editor, file ? (Pocket.readFile(file) || "") : "");
  Pocket.setText(status, file ? `Opened: ${file}` : "New note");
  show(listObjects, false); show(editObjects, true);
}
function saveNote() {
  const requested = normalizedName(Pocket.getText(nameField));
  if (activeFile && activeFile !== requested && !Pocket.renameFile(activeFile, requested)) {
    Pocket.setText(status, "Name already exists"); Pocket.tone(180, 120); return;
  }
  const ok = Pocket.writeFile(requested, Pocket.getText(editor));
  if (ok) { activeFile = requested; Pocket.setText(nameField, requested.replace(/\.txt$/i, "")); }
  Pocket.setText(status, ok ? `Saved: ${requested}` : "Save failed");
  Pocket.tone(ok ? 880 : 180, ok ? 35 : 120);
}
// Always open on the file list. Loading the first note automatically made the
// app look empty until List was pressed and was surprising after a restart.
showList();
