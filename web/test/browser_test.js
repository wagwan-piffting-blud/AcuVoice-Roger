// browser_test.js - headless Chrome test of the REAL browser path:
// Worker + WASM reading its data from the preloaded acu.data bundle. Synthesizes the 3
// fixtures in-browser and compares the µ-law output byte-for-byte against the native golden WAVs.
const http = require('http'), fs = require('fs'), path = require('path');
const puppeteer = require('puppeteer-core');

const ROOT = path.join(__dirname, '..', 'site');
const PORT = 8753;
const CHROME = process.env.CHROME ||
  ['C:/Program Files/Google/Chrome/Application/chrome.exe',
   'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe']
   .find(p => fs.existsSync(p));

const MIME = {'.html':'text/html','.js':'text/javascript','.wasm':'application/wasm','.data':'application/octet-stream','.wav':'audio/wav'};
function startServer(){
  return new Promise(res=>{
    const s = http.createServer((req,resp)=>{
      let p = decodeURIComponent(req.url.split('?')[0]); if(p==='/')p='/index.html';
      const file = path.join(ROOT,p);
      if(!file.startsWith(ROOT)||!fs.existsSync(file)||fs.statSync(file).isDirectory()){resp.writeHead(404);resp.end();return;}
      const size=fs.statSync(file).size, type=MIME[path.extname(file).toLowerCase()]||'application/octet-stream', range=req.headers.range;
      resp.setHeader('Accept-Ranges','bytes');
      if(range){ const m=/bytes=(\d+)-(\d*)/.exec(range); const a=+m[1], b=m[2]?+m[2]:size-1;
        resp.writeHead(206,{'Content-Type':type,'Content-Range':`bytes ${a}-${b}/${size}`,'Content-Length':b-a+1});
        fs.createReadStream(file,{start:a,end:b}).pipe(resp);
      } else { resp.writeHead(200,{'Content-Type':type,'Content-Length':size}); fs.createReadStream(file).pipe(resp); }
    });
    s.listen(PORT,()=>res(s));
  });
}

function readWavPcm(p){ const b=fs.readFileSync(p); const n=(b.length-44)/2; const a=new Int16Array(n); for(let i=0;i<n;i++)a[i]=b.readInt16LE(44+i*2); return a; }
const ULAW=new Int16Array(256);
for(let i=0;i<256;i++){const x=(~i)&0xff,s=x&0x80,e=(x>>4)&7,m=x&0x0f;let v=(((m<<3)+0x84)<<e)-0x84;ULAW[i]=s?-v:v;}

(async()=>{
  if(!CHROME){ console.error('No Chrome/Edge found'); process.exit(2); }
  const server = await startServer();
  const browser = await puppeteer.launch({ executablePath: CHROME, headless: 'new',
    args:['--no-sandbox','--autoplay-policy=no-user-gesture-required'] });
  const page = await browser.newPage();
  page.on('console', m=>console.error('[page]', m.text()));
  page.on('pageerror', e=>console.error('[pageerror]', e.message));
  await page.goto(`http://localhost:${PORT}/index.html`, { waitUntil:'domcontentloaded' });
  console.error('[test] waiting for engine ready...');
  await page.waitForFunction(()=>document.getElementById('status').className.includes('ok'), { timeout:180000 });
  console.error('[test] engine ready. synthesizing in-browser...');

  const EAS = fs.existsSync('C:/tmp/roger_smoke/roger.txt') ? fs.readFileSync('C:/tmp/roger_smoke/roger.txt','utf8').trim() : null;
  const cases = { hello:'Hello.', numbers:'one two three', alert:'Tornado warning for your county until 8:45 PM.' };
  if (EAS) cases.eas = EAS;   // long multi-sentence EAS message (exercises chunking + the file-handle fix)
  const out = await page.evaluate(async (cases)=>{
    const w = new Worker('worker.js');
    await new Promise(r=>{ const h=e=>{ if(e.data.type==='ready'){ w.removeEventListener('message',h); r(); } }; w.addEventListener('message',h); });
    const res = {};
    for(const k of Object.keys(cases)){
      const ab = await new Promise(r=>{ const h=e=>{ if(e.data.type==='audio'){ w.removeEventListener('message',h); r(e.data.ulaw); } }; w.addEventListener('message',h); w.postMessage({type:'synth',id:1,text:cases[k]}); });
      res[k] = ab ? Array.from(new Uint8Array(ab)) : null;
    }
    return res;
  }, cases);

  let allOk = true;
  for(const k of Object.keys(cases)){
    const ulaw = out[k];
    if(!ulaw){ console.log(`${k.padEnd(8)} -> NO AUDIO`); allOk=false; continue; }
    // golden: native fixtures for the short cases; the node-WASM render for the long EAS case
    const goldenPath = (k==='eas') ? path.join(__dirname,'..','emu','wasm_eas.wav')
                                   : path.join(__dirname,'..','native','fixtures',`${k}.wav`);
    const golden = readWavPcm(goldenPath);
    let diff=0; const m=Math.min(golden.length, ulaw.length);
    for(let i=0;i<m;i++) if(golden[i]!==ULAW[ulaw[i]]) diff++;
    const exact = golden.length===ulaw.length && diff===0;
    console.log(`${k.padEnd(8)} golden=${golden.length} browser=${ulaw.length} diff=${diff} -> ${exact?'EXACT':'MISMATCH'}`);
    if(!exact) allOk=false;
  }
  await browser.close(); server.close();
  console.log(allOk ? '\nALL EXACT - browser path verified.' : '\nFAILURES present.');
  process.exit(allOk?0:1);
})();
