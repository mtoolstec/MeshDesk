const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('apps/deepseek/app.js', 'utf8');
const readConfig = source.slice(source.indexOf('let configMigrationFailed'), source.indexOf('function setMessage'));
function run(stored, legacy, succeeds = true) {
 const calls = [];
 const context = {CONFIG_FILE:'config.json',Pocket:{
  readDataFile:()=>stored,readFile:()=>legacy,
  writeDataFile:(name,value)=>{calls.push('data');if(succeeds)stored=value;return succeeds},
  writeFile:()=>{calls.push('legacy-clear');return true}
 },setMessage:()=>{}};
 vm.createContext(context);vm.runInContext(readConfig,context);
 return {value:context.readConfig(),calls};
}
test('DeepSeek migrates and verifies before clearing legacy credentials',()=>{
 const legacy=JSON.stringify({apiKey:'test-key',baseUrl:'https://example.invalid'});
 const ok=run(null,legacy);assert.equal(ok.value.apiKey,'test-key');assert.deepEqual(ok.calls,['data','legacy-clear']);
 const failed=run(null,legacy,false);assert.equal(failed.value,null);assert.deepEqual(failed.calls,['data']);
 const current=run(JSON.stringify({apiKey:'new-key'}),legacy);assert.equal(current.value.apiKey,'new-key');assert.deepEqual(current.calls,[]);
 assert.equal(run('{invalid',legacy).value,null);
 assert.equal(run(null,'{}').value,null);
});

test('setup QR uses current IP, reuses one widget and hides it offline',()=>{
 let ip='192.0.2.1',creates=0,updates=[],visible=new Map(),text=[];
 const context={TEXT:'#fff',status:90,balanceLabel:91,balance:92,detail:93,Pocket:{
 device:()=>({ipAddress:ip}),text:()=>80,setText:(id,value)=>text.push(value),
 setVisible:(id,value)=>visible.set(id,value),
 qrCode:(url)=>{creates++;updates.push(url);return 81},setQrCode:(id,url)=>updates.push(url)
 }};
 vm.createContext(context);vm.runInContext(source.slice(source.indexOf('let setupHint'),source.indexOf('let configMigrationFailed')),context);
 context.showSetup(true);assert.equal(creates,1);assert.equal(updates[0],'http://192.0.2.1/app-config?id=deepseek');
 context.showSetup(true);assert.equal(creates,1);assert.equal(updates.length,1);
 ip='192.0.2.2';context.showSetup(true);assert.equal(updates[1],'http://192.0.2.2/app-config?id=deepseek');
 ip='0.0.0.0';context.showSetup(true);assert.equal(visible.get(81),false);
 context.showSetup(false);assert.equal(visible.get(90),true);assert.equal(visible.get(80),false);
});
