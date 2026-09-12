// Independent Electron fixture: synthetic content only; never opens a user page.
// Run with the test-only Electron runtime, integration exe and output prefix.
const {app, BrowserWindow, ipcMain} = require('electron');
const fs = require('node:fs');
const path = require('node:path');
const {spawn} = require('node:child_process');
const [integration, output] = process.argv.slice(2);
const largeFirst=process.argv.includes('--large-first-wheel');
if (!integration || !output) throw new Error('Expected integration executable and output prefix');
const prefix = path.resolve(output);
fs.writeFileSync(prefix+'-versions.json',JSON.stringify(process.versions,null,2));
app.setPath('userData', path.join(path.dirname(prefix), 'electron-scroll-profile'));
app.commandLine.appendSwitch('force-device-scale-factor', '1');
app.commandLine.appendSwitch('disable-background-timer-throttling');
let window, child;
process.on('uncaughtException', error => {fs.writeFileSync(prefix+'-fixture-error.txt',error.stack);app.exit(1);});
ipcMain.on('stats', (_, stats) => {
  fs.writeFileSync(prefix + '-stats.tmp', stats.join(' '));
  fs.renameSync(prefix + '-stats.tmp', prefix + '-stats.txt');
});
ipcMain.once('pixels', (_, rgba) => {
  const bgra=Buffer.from(rgba);for(let i=0;i<bgra.length;i+=4){const red=bgra[i];bgra[i]=bgra[i+2];bgra[i+2]=red;}
  fs.writeFileSync(prefix+'-expected.bgra',bgra);
});
ipcMain.once('ready', () => {
  window.show(); window.focus();
  setTimeout(() => {
    const handle=window.getNativeWindowHandle();
    const hwnd=handle.length>=8?handle.readBigUInt64LE():BigInt(handle.readUInt32LE());
    const log=fs.openSync(prefix+'-native.log','w');
    child = spawn(path.resolve(integration), ['electron-scroll-test', String(hwnd), prefix], {stdio:['ignore',log,log]});
    child.on('error',error=>{fs.writeFileSync(prefix+'-fixture-error.txt',error.stack);app.exit(1);});
    child.on('exit', code => {app.exit(code ?? 1);});
  }, 600);
});
app.whenReady().then(() => {
  window = new BrowserWindow({title:'PcTool Electron scroll fixture',x:120,y:100,width:900,height:740,frame:false,show:false,webPreferences:{nodeIntegration:true,contextIsolation:false,backgroundThrottling:false}});
  window.loadURL('data:text/html;charset=utf-8,'+encodeURIComponent(`<!doctype html><meta charset="utf-8">
  <style>*{box-sizing:border-box}body{margin:0;background:#eceef0;font:18px "Segoe UI"}aside{position:absolute;left:20px;top:20px;width:180px}header{position:absolute;left:250px;top:30px}#list{position:absolute;left:300px;top:240px;width:480px;height:360px;overflow-y:scroll;background:white}canvas{display:block}#hover{position:absolute;left:240px;top:80px;width:570px;height:100px;background:#ddd}#hover:hover{height:140px}</style>
  <aside>Fixed navigation<br>固定侧栏</aside><header>Independent Electron wheel routing regression</header><div id="hover">Hover must remain blocked during capture</div><div id="list"><canvas width="450" height="2000"></canvas></div>
  <script>const {ipcRenderer}=require('electron');const list=document.querySelector('#list'),canvas=document.querySelector('canvas'),ctx=canvas.getContext('2d');
  ctx.fillStyle='white';ctx.fillRect(0,0,450,2000);for(let row=0;row<66;row++){ctx.fillStyle='#303030';ctx.font='16px Segoe UI';ctx.fillText('记录 '+row+'  Task '+(row*7919)+' complete / 长截图',12,row*30+20);ctx.fillStyle='#dcdcdc';ctx.fillRect(0,row*30+29,450,1);}
  let wheels=0,moves=0,clicks=0;document.addEventListener('wheel',e=>{wheels++;if(${largeFirst}&&wheels===1){e.preventDefault();list.scrollTop+=Math.sign(e.deltaY)*100;}},{passive:false});document.addEventListener('mousemove',()=>moves++);document.addEventListener('mousedown',()=>clicks++);
  ipcRenderer.send('pixels',ctx.getImageData(0,0,450,2000).data);
  list.scrollTop=600;setInterval(()=>ipcRenderer.send('stats',[list.scrollTop,wheels,moves,clicks]),40);setTimeout(()=>ipcRenderer.send('ready'),200);
  </script>`));
});
app.on('window-all-closed',()=>app.quit());
