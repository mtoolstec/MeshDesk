const assert = require("assert"), fs = require("fs"), vm = require("vm");
let revision = 1;
let messages = [{ index: 0, text: "hello mesh", peer: "Alice", direct: false, outgoing: false, contact: true }];
let next = 0; const widgets = new Map(), sent = [];
const widget = (type, values) => { const h = next++; widgets.set(h, { type, visible: true, text: "", ...values }); return h; };
const dataFiles = new Map();
const soundStyles=[0,0], previews=[];
const unreadState={groups:false,direct:false}; let chatView;
const Pocket = {
  notificationSound: (direct, style) => {if (style===undefined) return soundStyles[direct?1:0];soundStyles[direct?1:0]=style;return true;},
  previewNotificationSound: (direct,style) => {previews.push({direct,style});return true;},
  chatUnread: () => ({...unreadState}),
  markChatRead: direct => { unreadState[direct ? "direct" : "groups"]=false; return true; },
  setChatView: (direct,peerId,visible) => {chatView={direct,peerId,visible}; return true; },
  readDataFile: name => dataFiles.get(name) || null,
  writeDataFile: (name, text) => { dataFiles.set(name, text); return true; },
  readFile: () => "migrated", writeFile: () => true, communication: () => ({configurationProtocol:"meshtastic"}),
  device: () => ({ darkMode: true, language: "en" }), setBackground: () => {}, setScrollable: () => {}, log: () => {},
  scrollArea: (x,y,width,height) => widget("scroll",{x,y,width,height,scrollY:0}),
  setParent: (h,parent) => widgets.get(h).parent=parent,
  getScrollY: h => widgets.get(h).scrollY,
  scrollTo: (h,y) => widgets.get(h).scrollY=y,
  rect: (x,y,width,height,color) => widget("rect",{x,y,width,height,color}),
  text: (text,x,y,width,color) => widget("text",{text,x,y,width,color}),
  textArea: (x,y,width,height,maxLength,oneLine) => widget("textarea",{x,y,width,height,maxLength,oneLine}),
  grid: (x,y,columns,rows,cellSize,gap,color) => widget("grid",{x,y,columns,rows,cellSize,gap,color}),
  setGridCell: () => {},
  hitArea: (x,y,width,height,callback) => widget("hit",{x,y,width,height,callback}),
  longPressArea: (x,y,width,height,callback) => widget("long",{x,y,width,height,callback}),
  setTextAlign: (h,align) => widgets.get(h).align=align, setText: (h,text) => widgets.get(h).text=text,
  getText: h => widgets.get(h).text, setColor: (h,color) => widgets.get(h).color=color,
  setTextAreaOneLine: (h,oneLine) => widgets.get(h).oneLine=oneLine,
  setPosition: (h,x,y) => Object.assign(widgets.get(h),{x,y}), setSize: (h,width,height) => Object.assign(widgets.get(h),{width,height}),
  wrapText: (label, text, width, spacing) => {
    const lines=text.split("\n").reduce((count,line)=>count+Math.max(1,Math.ceil(line.length/43)),0);
    const height=27+(lines-1)*(27+spacing);
    Object.assign(widgets.get(label),{text,width,height,overflow:"wrap",spacing});return height;
  },
  fitText: (label, box) => { const l=widgets.get(label), b=widgets.get(box); Object.assign(l,{x:b.x+4,y:b.y+(b.height-27)/2,width:b.width-8,height:27,overflow:"ellipsis",align:"center"}); return true; },
  focus: h => { widgets.get(h).focused=true; },
  setVisible: (h,visible) => widgets.get(h).visible=visible,
  meshContacts: () => ["Alice"], meshMessages: () => messages.map((m,index)=>({...m,index})),
  meshMessageRevision: () => revision, meshDeleteMessage: index => { messages.splice(index,1); revision++; return true; },
  meshSend: (direct,peer,text) => { sent.push({direct,peer,text}); return true; }
};
const context = vm.createContext({ Pocket, Math });
vm.runInContext(fs.readFileSync("apps/chats/app.js","utf8"), context);
const input = [...widgets.values()].find(w => w.type === "textarea");
assert(input && input.oneLine && input.maxLength === 120, "Chats compact composer should be one line until expanded");
assert.equal([...widgets.values()].filter(w => w.type === "textarea").length, 1);
assert.equal([...widgets.values()].filter(w => w.type === "hit").length, 23, "load-more replaces two history controls; T9 keys must stay removed");
assert.equal(input.height, 28, "composer should use the compact height in every state");
assert([...widgets.values()].some(w => w.type === "grid" && w.x === 205 && w.y === 184), "expand action must use a rendered fullscreen icon");
assert.equal([...widgets.values()].some(w => w.type === "text" && w.text.includes("Tap input")), false, "the keyboard hint text must stay removed");
const send = [...widgets.values()].find(w => w.type === "hit" && w.x === 240 && w.y === 183);
const expand = [...widgets.values()].find(w => w.type === "hit" && w.x === 204 && w.y === 183);
expand.callback(); assert.equal(input.height,82); assert.equal(input.width,300); assert.equal(input.oneLine,false); assert(input.focused);
send.callback(); assert.equal(input.height,28); assert.equal(input.width,188); assert.equal(input.oneLine,true);
input.text = "full keyboard"; send.callback();
assert.deepEqual(sent, [{ direct:false, peer:"", text:"full keyboard" }]); assert.equal(input.text, "");
const longPress = [...widgets.values()].find(w => w.type === "long"); longPress.callback();
const copy = [...widgets.values()].find(w => w.type === "hit" && w.x === 24); copy.callback(); assert.equal(input.text,"hello mesh");
longPress.callback(); const remove = [...widgets.values()].find(w => w.type === "hit" && w.x === 119); remove.callback(); assert.equal(messages.length,0);
console.log("chats native full-keyboard, copy, delete, and send tests passed");

// A received private conversation must remain selectable without a saved contact.
messages.push({text:"private hello", peer:"Bob", direct:true, outgoing:false, contact:false}); revision++;
vm.runInContext("frame(1000); toggleMode(); nextPeer();", context);
assert([...widgets.values()].some(w => w.type === "text" && w.visible && w.text.endsWith("private hello")));

// Persistent history uses stable node/message IDs, including after a rename.
widgets.clear(); next = 0; revision++;
let peerName = "Alice";
messages = Array.from({length:12},(_,i)=>({id:i+1, text:"public "+(i+1), peerId:"!11223344", peer:peerName, port:1, direct:false}));
messages.push(...Array.from({length:6},(_,i)=>({id:i+13, text:"private "+(i+13), peerId:"!11223344", peer:peerName, port:1, direct:true})));
const removedIds = [];
Pocket.chatPeers = () => [{id:"!11223344",name:peerName}];
Pocket.chatStorage = () => ({ready:true,pending:0,error:""});
Pocket.chatHistory = o => messages.filter(m=>m.direct===o.direct && (!o.peerId || m.peerId===o.peerId) && (!o.before || m.id<o.before) && (o.includeData || m.port===1)).slice(-o.limit).map(m=>({...m,peer:peerName}));
Pocket.deleteChatMessage = id => {removedIds.push(id); messages=messages.filter(m=>m.id!==id); revision++;return true;};
const historyContext = vm.createContext({Pocket,Math});
vm.runInContext(fs.readFileSync("apps/chats/app.js","utf8"), historyContext);
assert([...widgets.values()].some(w=>w.text==="Alice: public 12"));
assert(![...widgets.values()].some(w=>w.text==="Earlier messages" || w.text==="Newer messages"));
const list=[...widgets.values()].find(w=>w.type==="scroll");
list.scrollY=0;vm.runInContext("frame(1500)",historyContext);
assert([...widgets.values()].some(w=>w.visible && w.text==="Alice: public 1"));
vm.runInContext("toggleMode()",historyContext);
assert([...widgets.values()].some(w=>w.text==="private 18"));
peerName="Renamed"; revision++; vm.runInContext("frame(2000)",historyContext);
assert([...widgets.values()].some(w=>w.text==="private 18"));
const persistentInput=[...widgets.values()].find(w=>w.type==="textarea");persistentInput.text="stable recipient";
[...widgets.values()].find(w=>w.type==="hit" && w.x===240 && w.y===183).callback();
assert.deepEqual(sent.at(-1),{direct:true,peer:"!11223344",text:"stable recipient"});
[...widgets.values()].find(w=>w.type==="long").callback();
[...widgets.values()].find(w=>w.type==="hit" && w.x===119 && w.y===72).callback();
assert.deepEqual(removedIds,[13]);
messages.push({id:19,peerId:"!11223344",port:67,dataHex:"00ff03",text:"",direct:true});revision++;
vm.runInContext("frame(3000)",historyContext);
assert(![...widgets.values()].some(w=>w.text.includes("Port 67:")));
console.log("Chats persistent pagination, stable identity across rename, durable ID deletion and protocol data filtering passed");

assert(![...widgets.values()].some(w=>w.visible && (w.text==="Older" || w.text==="Newer" || w.text.startsWith("Protocol data:"))));

// Match the device binding budget: exhausted bindings previously disabled Close.
const runtimeSource = fs.readFileSync("src/PocketJsRuntime.cpp", "utf8");
const bindingLimit = Number(runtimeSource.match(/MAX_BUTTONS = (\d+)/)[1]);
assert([...widgets.values()].filter(w=>w.type==="hit" || w.type==="long").length <= bindingLimit);
vm.runInContext("openTools()",historyContext);
assert([...widgets.values()].some(w=>w.visible && w.text==="Back to chat"));
[...widgets.values()].find(w=>w.type==="hit" && w.x===20 && w.y===172).callback();
assert([...widgets.values()].some(w=>w.visible && w.text==="private 18"));
vm.runInContext("openTools(); toggleNames(); closeTools()",historyContext);
assert([...widgets.values()].some(w=>w.visible && w.text==="private 18"));
assert.equal(JSON.parse(dataFiles.get("preferences.json")).shortNames,true);
messages.push({id:20,port:1,text:"other private",peerId:"!55667788",peer:"Bob",direct:true});
const countBeforeClear=messages.length;
vm.runInContext("openTools(); askClear(); cancelClear()",historyContext);
assert.equal(messages.length,countBeforeClear);
vm.runInContext("askClear(); startClear()",historyContext);
for (let now=4000;now<9000;now+=400) vm.runInContext(`frame(${now})`,historyContext);
assert.equal(messages.filter(m=>m.direct && m.peerId==="!11223344" && m.port===1).length,0);
assert.equal(messages.filter(m=>!m.direct).length,12);
assert(messages.some(m=>m.id===19),"protocol data must be retained");
assert(messages.some(m=>m.id===20),"other private chats must be retained");
console.log("Chats menu close, runtime binding budget, abbreviated names, clear cancellation and scoped clearing passed");

peerName="A very long contact name that must never wrap";revision++;
vm.runInContext("closeTools(); frame(10000)",historyContext);
const headerName=[...widgets.values()].find(w=>w.type==="text" && w.x===170 && w.y<5);
assert(headerName && headerName.overflow==="ellipsis" && headerName.height===27 && headerName.width===104);
assert.equal(headerName.y+headerName.height/2,4+24/2);
console.log("Chats single-line contact header and centered button labels passed");

unreadState.groups=true;unreadState.direct=true;
vm.runInContext("frame(11000)",historyContext);
const groupDot=[...widgets.values()].find(w=>w.type==="rect" && w.x===76 && w.y===2);
const directDot=[...widgets.values()].find(w=>w.type==="rect" && w.x===154 && w.y===2);
assert(groupDot.visible && directDot.visible);
[...widgets.values()].find(w=>w.type==="hit" && w.x===10 && w.y===4).callback();
assert(!groupDot.visible && directDot.visible);
assert.equal(chatView.direct,false);
[...widgets.values()].find(w=>w.type==="hit" && w.x===88 && w.y===4).callback();
assert(!directDot.visible && chatView.direct);
// Tapping the already selected tab marks read without switching conversations.
unreadState.direct=true;
[...widgets.values()].find(w=>w.type==="hit" && w.x===88 && w.y===4).callback();
assert(!unreadState.direct && chatView.direct);
console.log("Chats unread badges and idempotent tab selection passed");

// Contacts must hand off the selected stable ID, including duplicate names.
widgets.clear();next=0;let launchId="!55667788", launchCalls=0;
Pocket.chatPeers=()=>[{id:"!11223344",name:"Same name"},{id:"!55667788",name:"Same name"}];
Pocket.takeChatTarget=()=>{launchCalls++;const id=launchId;launchId=null;return id;};
const launchContext=vm.createContext({Pocket,Math});
vm.runInContext(fs.readFileSync("apps/chats/app.js","utf8"),launchContext);
assert.equal(launchCalls,1);
assert.deepEqual(chatView,{direct:true,peerId:"!55667788",visible:true});
const launchInput=[...widgets.values()].find(w=>w.type==="textarea");launchInput.text="contacts recipient test";
[...widgets.values()].find(w=>w.type==="hit" && w.x===240 && w.y===183).callback();
assert.deepEqual(sent.at(-1),{direct:true,peer:"!55667788",text:"contacts recipient test"});
console.log("Contacts launch opens and sends to the selected stable peer ID passed");

// Short names use the last four hexadecimal characters of the stable node ID.
assert.equal(vm.runInContext('displayName("Long name", "!a1b2c3d4")', launchContext), "c3d4");
assert.equal(vm.runInContext('displayName("Long name", "aabbccddeeff0011")', launchContext), "0011");
assert.equal(vm.runInContext('displayName("Alice", "Alice")', launchContext), "Alice");
// Sound types are independent for Groups and Direct and use a separate fixed-size panel.
vm.runInContext("openTools(); openSounds(); chooseSound(2)", launchContext);
assert.deepEqual(soundStyles,[0,2]);assert.deepEqual(previews.at(-1),{direct:true,style:2});
assert([...widgets.values()].some(w=>w.visible && w.text==="Direct notification sound"));
assert(![...widgets.values()].some(w=>w.visible && w.text==="Clear this chat..."));
vm.runInContext("closeSounds(); closeTools(); selectMode(false); openTools(); openSounds(); chooseSound(4)", launchContext);
assert.deepEqual(soundStyles,[4,2]);
vm.runInContext("closeSounds(); closeTools()", launchContext);
assert([...widgets.values()].some(w=>w.visible && w.text==="Groups"));
assert([...widgets.values()].filter(w=>w.type==="hit" || w.type==="long").length <= bindingLimit);
console.log("Direct names, hexadecimal short IDs, independent sound choices and menu return passed");

// 50-record batches prepend without losing the anchor; live messages keep old history.
widgets.clear();next=0;revision++;peerName="Alice";
Pocket.takeChatTarget=()=>null;
messages=Array.from({length:150},(_,i)=>({id:i+1,text:"batch "+(i+1),peerId:"!11223344",peer:"Alice",port:1,direct:false}));
const requests=[];const originalHistory=Pocket.chatHistory;
Pocket.chatHistory=o=>{requests.push({...o});return originalHistory(o);};
const batchContext=vm.createContext({Pocket,Math});
vm.runInContext(fs.readFileSync("apps/chats/app.js","utf8"),batchContext);
assert.equal(requests.at(-1).limit,50);
assert.equal(vm.runInContext("messages.length",batchContext),50);
const batchList=[...widgets.values()].find(w=>w.type==="scroll");
batchList.scrollY=0;vm.runInContext("frame(100)",batchContext);
assert([...widgets.values()].some(w=>w.visible && w.text.endsWith("batch 101")));
vm.runInContext("loadMore()",batchContext);
assert.equal(requests.at(-1).before,101);assert.equal(batchList.scrollY,50*21);
assert.equal(vm.runInContext("messages.length",batchContext),100);
assert([...widgets.values()].some(w=>w.visible && w.text.endsWith("batch 101")),"anchor must survive prepend");
batchList.scrollY=0;vm.runInContext("frame(200); loadMore()",batchContext);
assert.equal(requests.at(-1).before,51);assert.equal(vm.runInContext("messages.length",batchContext),150);
batchList.scrollY=0;vm.runInContext("frame(300); loadMore()",batchContext);
assert.equal(vm.runInContext("hasMore",batchContext),false);
messages.push({id:151,text:"new live message",peerId:"!11223344",port:1,direct:false});revision++;
vm.runInContext("frame(1000)",batchContext);
assert.equal(vm.runInContext("messages.length",batchContext),151);
assert.equal(batchList.scrollY,0,"incoming messages must not jump away from old history");
assert.equal(vm.runInContext("new Set(messages.map(m=>m.id)).size",batchContext),151);
assert([...widgets.values()].filter(w=>w.type==="hit" || w.type==="long").length <= bindingLimit);
assert(widgets.size<=256);
vm.runInContext("resetPage(); refresh(true)",batchContext);
assert.equal(vm.runInContext("messages.length",batchContext),50);
assert.equal(batchList.scrollY,24+50*21-147);
console.log("Chats scrolling, 50-record batches, anchor preservation, live merge and bounded widgets passed");

// Long English words wrap and increase the row height without losing text or overlapping the next row.
const synchronizedText="That's great. The message on APP can be synchronized with the remote device without losing any words.";
messages.push({id:152,text:synchronizedText,peerId:"!11223344",port:1,direct:false});revision++;
vm.runInContext("frame(1500)",batchContext);
const longLine=[...widgets.values()].find(w=>w.type==="text" && w.visible && w.text.endsWith(synchronizedText));
assert(longLine,"display fitting must not truncate the stored message by character count");
assert.equal(longLine.overflow,"wrap");
assert(longLine.height>27,"long messages must use multiple lines");
assert.equal(longLine.spacing,-6,"wrapped lines keep compact spacing");
const lastRow=vm.runInContext("layoutRows[layoutRows.length-1]",batchContext);
const previousRow=vm.runInContext("layoutRows[layoutRows.length-2]",batchContext);
assert.equal(lastRow.y,previousRow.y+previousRow.height);
assert.equal(lastRow.height,longLine.height-6);
assert.equal(longLine.x,10);assert(longLine.x+longLine.width<=306,"reserve the right edge for the scrollbar");
assert.equal(batchList.x,0);assert.equal(batchList.width,320);
console.log("Chats multiline wrapping, variable row heights and right-edge scrollbar space passed");

// Prepending older multiline messages shifts the anchor by their measured height.
batchList.scrollY=0;
vm.runInContext("frame(1800)",batchContext);
messages.find(m=>m.id===70).text=synchronizedText;
const beforePrepend=vm.runInContext("contentHeight",batchContext);
vm.runInContext("loadMore()",batchContext);
assert.equal(batchList.scrollY,vm.runInContext("contentHeight",batchContext)-beforePrepend);
assert(batchList.scrollY>50*21,"multiline older messages add their complete measured height");
assert([...widgets.values()].filter(w=>w.type==="hit" || w.type==="long").length<=bindingLimit);
console.log("Chats wrapped-history anchor and binding budget passed");
