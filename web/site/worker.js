// worker.js - runs the AcuVoice WASM engine off the main thread.
// The engine reads its data files via synchronous HTTP Range requests (allowed in Workers),
// fetching only the bytes it needs and caching them in 256 KB blocks. A whole session
// streams ~1-2 MB out of the 160 MB soundbank instead of downloading it all.

importScripts('acu.js');

const BLOCK = 262144;                 // 256 KB block granularity
const sizeCache = new Map();          // url -> total bytes
const blockCache = new Map();         // url -> Map(blockIndex -> Uint8Array)

let totalFetched = 0;

function fileSize(url){
  if(sizeCache.has(url)) return sizeCache.get(url);
  // Range probe: ask for byte 0; Content-Range gives the full size. 404 -> missing.
  const xhr = new XMLHttpRequest();
  xhr.open('GET', url, false);
  xhr.setRequestHeader('Range', 'bytes=0-0');
  xhr.responseType = 'arraybuffer';
  try { xhr.send(); } catch(e){ sizeCache.set(url,-1); return -1; }
  if(xhr.status === 404){ sizeCache.set(url,-1); return -1; }
  let size = -1;
  const cr = xhr.getResponseHeader('Content-Range'); // "bytes 0-0/12345"
  if(cr){ const m = /\/(\d+)$/.exec(cr); if(m) size = parseInt(m[1],10); }
  if(size < 0){ // server ignored Range and sent the whole file (status 200)
    const cl = xhr.getResponseHeader('Content-Length');
    if(cl) size = parseInt(cl,10);
    if(xhr.status === 200 && xhr.response){ // cache the whole file we just got
      cacheWholeFile(url, new Uint8Array(xhr.response)); if(size<0) size = xhr.response.byteLength;
    }
  }
  sizeCache.set(url, size);
  return size;
}

function cacheWholeFile(url, bytes){
  let bm = blockCache.get(url); if(!bm){ bm = new Map(); blockCache.set(url, bm); }
  for(let b=0; b*BLOCK < bytes.length; b++) bm.set(b, bytes.subarray(b*BLOCK, Math.min((b+1)*BLOCK, bytes.length)));
  sizeCache.set(url, bytes.length);
}

function fetchBlock(url, b){
  let bm = blockCache.get(url); if(!bm){ bm = new Map(); blockCache.set(url, bm); }
  if(bm.has(b)) return bm.get(b);
  const size = fileSize(url);
  const start = b*BLOCK, end = Math.min(start+BLOCK, size) - 1;
  const xhr = new XMLHttpRequest();
  xhr.open('GET', url, false);
  xhr.setRequestHeader('Range', 'bytes='+start+'-'+end);
  xhr.responseType = 'arraybuffer';
  xhr.send();
  const buf = new Uint8Array(xhr.response);
  totalFetched += buf.length;
  if(xhr.status === 200 && buf.length === size){ // server ignored Range; cache whole
    cacheWholeFile(url, buf); return blockCache.get(url).get(b);
  }
  bm.set(b, buf);
  return buf;
}

function fileRead(url, pos, len, dst){
  const size = fileSize(url); if(size < 0) return 0;
  if(pos >= size) return 0;
  const n = Math.min(len, size - pos);
  const out = new Uint8Array(Module.HEAPU8.buffer, dst, n);
  let done = 0;
  while(done < n){
    const abs = pos + done;
    const b = Math.floor(abs / BLOCK);
    const blk = fetchBlock(url, b);
    const off = abs - b*BLOCK;
    const take = Math.min(blk.length - off, n - done);
    out.set(blk.subarray(off, off+take), done);
    done += take;
    if(take <= 0) break;
  }
  return done;
}

let Module = null;
AcuModule({
  acuFileSize: fileSize,
  acuFileRead: (url, pos, len, dst) => fileRead(url, pos, len, dst),
}).then(M => {
  Module = M;
  const ok = M.ccall('acu_boot_wasm', 'number', [], []);
  postMessage({ type: 'ready', ok: !!ok, fetched: totalFetched });
});

onmessage = (e) => {
  const msg = e.data;
  if(msg.type === 'synth'){
    const t0 = (self.performance ? performance.now() : 0);
    const len = Module.ccall('acu_synth_wasm', 'number', ['string'], [msg.text]);
    let ulaw = null;
    if(len > 0){
      const ptr = Module.ccall('acu_ulaw_ptr', 'number', [], []);
      ulaw = Module.HEAPU8.slice(ptr, ptr + len);   // copy out (detaches from heap)
    }
    const ms = (self.performance ? performance.now() - t0 : 0);
    postMessage({ type: 'audio', id: msg.id, ulaw: ulaw ? ulaw.buffer : null, len, ms, fetched: totalFetched },
                ulaw ? [ulaw.buffer] : []);
  }
};
