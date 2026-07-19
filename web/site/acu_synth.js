// app.js - main thread: drive the worker, play µ-law via WebAudio, offer WAV download.
'use strict';

// G.711 µ-law -> PCM16 table
const ULAW = new Int16Array(256);
for(let i=0;i<256;i++){ const x=(~i)&0xff, s=x&0x80, e=(x>>4)&7, m=x&0x0f; let v=(((m<<3)+0x84)<<e)-0x84; ULAW[i]=s?-v:v; }

const SR = 8000;
let audioCtx = null;
let worker = null;
let ready = false;
let lastPcm = null;       // Int16Array of last render (for download)
let reqId = 0;
let curSource = null;

const $ = (id) => document.getElementById(id);
function setStatus(msg, cls){ const s=$('status'); s.textContent = msg; s.className = 'status' + (cls?(' '+cls):''); }

function boot(){
  setStatus('Loading engine + voicebank (~160 MB, one-time download)...');
  worker = new Worker('worker.js');
  worker.onmessage = (e)=>{
    const m = e.data;
    if(m.type === 'ready'){
      ready = m.ok;
      if(m.ok){ setStatus('Ready - Roger is online.', 'ok'); $('speak').disabled=false; }
      else setStatus('Engine failed to initialize.', 'err');
    } else if(m.type === 'audio'){
      onAudio(m);
    }
  };
  worker.onerror = (err)=>{ setStatus('Worker error: '+err.message, 'err'); };
}

function speak(){
  if(!ready) return;
  const text = $('text').value.trim();
  if(!text){ setStatus('Type something for Roger to say.', 'err'); return; }
  if(!audioCtx) audioCtx = new (window.AudioContext||window.webkitAudioContext)();
  if(audioCtx.state === 'suspended') audioCtx.resume();
  $('speak').disabled = true;
  setStatus('Synthesizing...');
  worker.postMessage({ type:'synth', id:++reqId, text });
}

function onAudio(m){
  $('speak').disabled = false;
  if(!m.len || !m.ulaw){ setStatus('Synthesis produced no audio.', 'err'); return; }
  const ulaw = new Uint8Array(m.ulaw);
  const pcm = new Int16Array(ulaw.length);
  const f32 = new Float32Array(ulaw.length);
  for(let i=0;i<ulaw.length;i++){ const v = ULAW[ulaw[i]]; pcm[i]=v; f32[i]=v/32768; }
  lastPcm = pcm;

  // play
  stopAudio();
  const buf = audioCtx.createBuffer(1, f32.length, SR);
  buf.getChannelData(0).set(f32);
  const src = audioCtx.createBufferSource();
  src.buffer = buf; src.connect(audioCtx.destination);
  src.onended = ()=>{ if(curSource===src) curSource=null; };
  src.start();
  curSource = src;

  const secs = (m.len/SR).toFixed(2);
  setStatus(`Spoke ${m.len.toLocaleString()} samples (${secs}s) in ${m.ms.toFixed(0)} ms`, 'ok');
  $('download').disabled = false;
}

function stopAudio(){ if(curSource){ try{curSource.stop();}catch(e){} curSource=null; } }

function downloadWav(){
  if(!lastPcm) return;
  const n = lastPcm.length, db = n*2;
  const buf = new ArrayBuffer(44+db), dv = new DataView(buf);
  const wr = (o,s)=>{ for(let i=0;i<s.length;i++) dv.setUint8(o+i, s.charCodeAt(i)); };
  wr(0,'RIFF'); dv.setUint32(4,36+db,true); wr(8,'WAVE'); wr(12,'fmt ');
  dv.setUint32(16,16,true); dv.setUint16(20,1,true); dv.setUint16(22,1,true);
  dv.setUint32(24,SR,true); dv.setUint32(28,SR*2,true); dv.setUint16(32,2,true); dv.setUint16(34,16,true);
  wr(36,'data'); dv.setUint32(40,db,true);
  for(let i=0;i<n;i++) dv.setInt16(44+i*2, lastPcm[i], true);
  const blob = new Blob([buf], {type:'audio/wav'});
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob); a.download = 'roger.wav'; a.click();
  setTimeout(()=>URL.revokeObjectURL(a.href), 5000);
}

window.addEventListener('DOMContentLoaded', ()=>{
  $('speak').addEventListener('click', speak);
  $('stop').addEventListener('click', stopAudio);
  $('download').addEventListener('click', downloadWav);
  document.querySelectorAll('.preset').forEach(b=> b.addEventListener('click', ()=>{ $('text').value=b.dataset.text; }));
  $('text').addEventListener('keydown', (e)=>{ if(e.key==='Enter' && (e.ctrlKey||e.metaKey)) speak(); });
  boot();
});
