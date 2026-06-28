# build_wasm.ps1 - compile the emulator to WebAssembly (run from PowerShell in web/emu).
# Output goes to ../site/ (the GitHub Pages root): acu.js, acu.wasm, acu.data (preloaded DLL).
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
New-Item -ItemType Directory -Force -Path ..\site | Out-Null

$exportedFns = "['_acu_boot_wasm','_acu_synth_wasm','_acu_ulaw_ptr','_malloc','_free']"
$exportedRt  = "['ccall','cwrap','UTF8ToString','stringToUTF8','lengthBytesUTF8','HEAPU8']"

emcc mem.c cpu.c loader.c win32.c vfs_wasm.c host.c wasm_main.c `
  -O2 -lm `
  -o ..\site\acu.js `
  --preload-file ..\..\lib\avcore_acu.dll@avcore_acu.dll `
  -s MODULARIZE=1 -s "EXPORT_NAME=AcuModule" `
  -s ALLOW_MEMORY_GROWTH=1 -s INITIAL_MEMORY=16777216 -s MAXIMUM_MEMORY=268435456 `
  -s STACK_SIZE=2097152 `
  -s "EXPORTED_FUNCTIONS=$exportedFns" `
  -s "EXPORTED_RUNTIME_METHODS=$exportedRt" `
  -s "ENVIRONMENT=web,worker,node" `
  -s "EXPORT_ES6=0"

if ($LASTEXITCODE -ne 0) { Write-Error "emcc build failed"; exit 1 }
Write-Host "built ../site/acu.js  ($((Get-Item ..\site\acu.wasm).Length) bytes wasm, $((Get-Item ..\site\acu.data).Length) bytes data)"
