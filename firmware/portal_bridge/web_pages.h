// Admin page HTML. Kept small and dependency-free: no CDN, since the phone
// viewing it may be connected only to this board with no internet.
#pragma once

static const char PAGE_HEAD[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Portal Bridge</title>
<style>
:root{--bg:#f6f7f9;--fg:#1b1f24;--card:#fff;--mut:#5c6670;--line:#dde1e6;--acc:#1f6feb;--ok:#1a7f37;--bad:#cf222e;--warn:#9a6700}
@media (prefers-color-scheme:dark){:root{--bg:#0d1117;--fg:#e6edf3;--card:#161b22;--mut:#8b949e;--line:#30363d;--acc:#4493f8;--ok:#3fb950;--bad:#f85149;--warn:#d29922}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
header{padding:12px 16px;border-bottom:1px solid var(--line);background:var(--card)}
header b{font-size:17px}header small{color:var(--mut);display:block}
nav{display:flex;flex-wrap:wrap;gap:4px 14px;padding:8px 16px;border-bottom:1px solid var(--line);background:var(--card)}
nav a{color:var(--acc);text-decoration:none}
main{padding:16px;max-width:760px;margin:auto}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px;margin-bottom:14px}
h2{font-size:16px;margin:0 0 10px}
table{width:100%;border-collapse:collapse}td{padding:4px 0;vertical-align:top;border-bottom:1px solid var(--line)}
td:first-child{color:var(--mut);width:42%;padding-right:8px}
.ok{color:var(--ok)}.bad{color:var(--bad)}.warn{color:var(--warn)}.mut{color:var(--mut)}
input,textarea,button{font:inherit;padding:9px;border:1px solid var(--line);border-radius:8px;background:var(--bg);color:var(--fg);width:100%}
button{background:var(--acc);color:#fff;border:0;cursor:pointer;margin-top:8px}
label{display:block;margin-top:10px;color:var(--mut)}
pre{white-space:pre-wrap;word-break:break-all;font-size:12px;background:var(--bg);padding:10px;border-radius:8px;max-height:60vh;overflow:auto}
.sms{border-bottom:1px solid var(--line);padding:8px 0}.sms:last-child{border:0}
</style></head><body>
)HTML";

static const char PAGE_NAV[] PROGMEM = R"HTML(<nav><a href="/">Status</a><a href="/sms">SMS</a><a href="/at">Modem console</a>
<a href="/log">Log</a><a href="/report.txt">Download report</a><a href="/update">Firmware</a><a href="/setup">Passwords</a></nav>)HTML";

static const char PAGE_STATUS[] PROGMEM = R"HTML(<main>
<div class="card"><h2>Modem &amp; SIM</h2><table id="modem"><tr><td>Loading…</td><td></td></tr></table></div>
<div class="card"><h2>Board</h2><table id="board"></table></div>
<div class="card"><h2>Hotspot</h2><table id="ap"></table></div>
<form method="post" action="/reboot" onsubmit="return confirm('Reboot the board?')"><button>Reboot</button></form>
</main>
<script>
function row(k,v,c){return '<tr><td>'+k+'</td><td class="'+(c||'')+'">'+v+'</td></tr>'}
function esc(s){return String(s==null?'':s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
async function tick(){
 try{const r=await fetch('/api/status');const s=await r.json();const m=s.modem;
 let h=row('Modem',m.alive?'answering':'not answering — check the 4G DIP switch',m.alive?'ok':'bad');
 if(m.alive){h+=row('Model / firmware',esc(m.model+' / '+m.revision))+row('SIM',esc(m.sim),m.sim=='READY'?'ok':'bad')
 +row('Own number (from SIM)',esc(m.own_number||'not stored on SIM'))+row('Operator',esc(m.operator||'-'))
 +row('Signal',m.csq==99?'unknown':(m.rssi_dbm+' dBm (CSQ '+m.csq+')'),m.csq==99||m.csq<10?'warn':'ok')
 +row('LTE registration',esc(m.reg_eps_text),(m.reg_eps==1||m.reg_eps==5)?'ok':'warn')
 +row('2G/CS registration',esc(m.reg_cs_text))+row('Radio',esc(m.cpsi))+row('SMS storage',esc(m.sms_storage))
 +row('SMS notifications since boot',m.sms_notifications)+row('IMEI',esc(m.imei))+row('ICCID',esc(m.iccid));}
 document.getElementById('modem').innerHTML=h;
 const b=s.board;document.getElementById('board').innerHTML=row('Firmware',esc(b.fw))+row('Uptime',b.uptime_s+' s')
 +row('Last reset',esc(b.reset_reason))+row('PSRAM',b.psram_kb+' KB')+row('Flash',b.flash_mb+' MB')+row('Free heap',b.heap_kb+' KB')
 +row('Battery',b.battery_found?(b.battery_v.toFixed(2)+' V, '+b.battery_pct.toFixed(0)+' %'):'gauge not found','');
 const a=s.ap;document.getElementById('ap').innerHTML=row('Name',esc(a.ssid))+row('Admin address',esc(a.ip))+row('Connected devices',a.clients);
 }catch(e){}
}
tick();setInterval(tick,5000);
</script>)HTML";

static const char PAGE_SMS[] PROGMEM = R"HTML(<main><div class="card"><h2>Messages on the SIM / modem</h2>
<p class="mut">Read-only. Nothing is deleted. Opening this page marks messages as read in the modem.</p>
<div id="list">Loading… (can take a few seconds)</div></div></main>
<script>
function esc(s){return String(s==null?'':s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
fetch('/api/sms').then(r=>r.json()).then(a=>{
 if(!a.length){document.getElementById('list').textContent='No messages stored.';return}
 document.getElementById('list').innerHTML=a.reverse().map(m=>m.error?
 '<div class="sms bad">#'+m.index+': could not decode ('+esc(m.error)+')</div>':
 '<div class="sms"><b>'+esc(m.from)+'</b> <span class="mut">'+esc(m.time)+(m.parts>1?' · part '+m.part+'/'+m.parts:'')+'</span><br>'+esc(m.text)+'</div>').join('');
}).catch(e=>{document.getElementById('list').textContent='Failed: '+e});
</script>)HTML";

static const char PAGE_AT[] PROGMEM = R"HTML(<main><div class="card"><h2>Modem console</h2>
<p class="mut">Sends one AT command to the A7670E and shows the reply.</p>
<input id="c" value="AT+CSQ" autocapitalize="characters" autocomplete="off"><button onclick="go()">Send</button>
<pre id="o"></pre></div></main>
<script>
async function go(){const c=document.getElementById('c').value;const o=document.getElementById('o');
o.textContent='> '+c+'\n…\n'+o.textContent;
const r=await fetch('/api/at',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'cmd='+encodeURIComponent(c)});
o.textContent='> '+c+'\n'+await r.text()+'\n\n'+o.textContent.replace(/^> .*\n…\n/,'');}
document.getElementById('c').addEventListener('keydown',e=>{if(e.key=='Enter')go()});
</script>)HTML";

static const char PAGE_UPDATE[] PROGMEM = R"HTML(<main><div class="card"><h2>Firmware update</h2>
<p>Choose <b>portal-bridge-ota.bin</b> from the GitHub release (not the <i>-full</i> file). The board reboots when done.</p>
<form method="post" action="/update" enctype="multipart/form-data">
<input type="file" name="fw" accept=".bin" required><button>Upload and install</button></form></div></main>)HTML";
