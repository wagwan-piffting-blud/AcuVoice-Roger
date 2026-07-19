// worker.js - runs the AcuVoice WASM engine off the main thread.
// The entire data tree (dictionaries + ~160 MB soundbank) is preloaded from acu.data
// into Emscripten's in-memory FS, so the engine reads its files with plain stdio - no
// network fetches at synth time. Emscripten fetches acu.data once before the module
// promise resolves, so MEMFS is fully populated by the time we boot.

importScripts('acu.js');

let Module = null;
AcuModule().then(M => {
  Module = M;
  const ok = M.ccall('acu_boot_wasm', 'number', [], []);
  postMessage({ type: 'ready', ok: !!ok });
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
    postMessage({ type: 'audio', id: msg.id, ulaw: ulaw ? ulaw.buffer : null, len, ms },
                ulaw ? [ulaw.buffer] : []);
  }
};
