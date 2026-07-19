# prep_data.ps1 - assemble the browser data tree (lowercased filenames). This staged tree
# is what build_wasm.ps1 preloads into the self-contained acu.data bundle (and what the
# node/browser tests read). Source = repo data/.  Dest = web/site/data/ (gitignored).
# The engine hardcodes lowercase names and MEMFS/Pages are case-sensitive, so everything
# is lowercased here. build_wasm.ps1 auto-runs this if the soundbank isn't staged yet.
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$src  = Join-Path $repo 'data'
$dst  = Join-Path $PSScriptRoot 'site\data'

function CopyLower($subSrc, $subDst, $excludeDirs){
    $s = Join-Path $src $subSrc
    $d = Join-Path $dst $subDst
    New-Item -ItemType Directory -Force -Path $d | Out-Null
    Get-ChildItem -Path $s -File | ForEach-Object {
        $target = Join-Path $d ($_.Name.ToLower())
        Copy-Item $_.FullName $target -Force
    }
}

CopyLower 'Dictfls'  'dictfls'  @()
CopyLower 'Ulaw08Sb' 'ulaw08sb' @()
New-Item -ItemType Directory -Force -Path (Join-Path $dst 'temp') | Out-Null

$bytes = (Get-ChildItem -Path $dst -Recurse -File | Measure-Object -Property Length -Sum).Sum
"prepared $dst  ($([math]::Round($bytes/1MB,1)) MB)"
Get-ChildItem -Path (Join-Path $dst 'ulaw08sb') | Select-Object Name,Length | Format-Table -AutoSize | Out-String -Width 120
