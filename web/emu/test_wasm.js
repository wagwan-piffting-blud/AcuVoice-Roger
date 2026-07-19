// test_wasm.js - node smoke test for the WASM build. Boots the engine reading its data
// straight from the preloaded acu.data bundle (dictionaries + soundbank live in MEMFS),
// synthesizes, and writes a WAV to compare against the native fixtures.
//   node test_wasm.js "text" out.wav
const fs = require('fs');
const path = require('path');
const AcuModule = require('../site/acu.js');

let Module;
const opts = {
  // find acu.wasm / acu.data next to acu.js in ../site; Emscripten reads the .data via fs.
  locateFile: (p) => path.join(__dirname, '..', 'site', p),
};

function ulawToPcm(u){ // standard G.711 µ-law decode
  const t = new Int16Array(256);
  for(let i=0;i<256;i++){ const x=(~i)&0xff,s=x&0x80,e=(x>>4)&7,m=x&0x0f; let v=(((m<<3)+0x84)<<e)-0x84; t[i]=s?-v:v; }
  const out = new Int16Array(u.length); for(let i=0;i<u.length;i++) out[i]=t[u[i]]; return out;
}
function writeWav(file, pcm){
  const sr=8000, db=pcm.length*2, hdr=Buffer.alloc(44);
  hdr.write('RIFF',0); hdr.writeUInt32LE(36+db,4); hdr.write('WAVE',8); hdr.write('fmt ',12);
  hdr.writeUInt32LE(16,16); hdr.writeUInt16LE(1,20); hdr.writeUInt16LE(1,22); hdr.writeUInt32LE(sr,24);
  hdr.writeUInt32LE(sr*2,28); hdr.writeUInt16LE(2,32); hdr.writeUInt16LE(16,34); hdr.write('data',36); hdr.writeUInt32LE(db,40);
  const body=Buffer.from(pcm.buffer, pcm.byteOffset, db);
  fs.writeFileSync(file, Buffer.concat([hdr, body]));
}

AcuModule(opts).then(M => {
  Module = M;
  const ok = M.ccall('acu_boot_wasm','number',[],[]);
  console.error('[wasm] boot ok =', ok);
  if(!ok){ console.error('BOOT FAILED'); process.exit(1); }
  const text = process.argv[2] || 'Tornado warning for your county until 8:45 PM.';
  const len = M.ccall('acu_synth_wasm','number',['string'],[text]);
  console.error('[wasm] ulaw bytes =', len, '(', (len/8000).toFixed(2), 's )');
  if(!len){ console.error('SYNTH FAILED'); process.exit(1); }
  const ptr = M.ccall('acu_ulaw_ptr','number',[],[]);
  const ulaw = M.HEAPU8.slice(ptr, ptr+len);
  writeWav(process.argv[3] || 'wasm_out.wav', ulawToPcm(ulaw));
  console.error('[wasm] wrote', process.argv[3] || 'wasm_out.wav');
});
