param([ValidateSet('before','candidate')][string]$Label='before',[string]$Rom)
$ErrorActionPreference='Stop'
$vmadRoot=[System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$vmadBuild=Join-Path $vmadRoot 'build'
$vmadExe=Join-Path $vmadBuild 'default/tools/n64_runner/Release/ezy_n64_runner.exe'
if(!$Rom) {$Rom=Join-Path $vmadRoot 'roms/Mario Kart 64 (USA).z64'}
if(!(Test-Path -LiteralPath $Rom -PathType Leaf)) {throw "Local ROM not found: $Rom"}
foreach($vmadFrames in @(120,300,600)) {
    $vmadState=Join-Path $vmadBuild "vmad-$Label-$vmadFrames.bin"
    & $vmadExe $Rom --rsp-interpreter --rdp-sync --dump-state $vmadState $vmadFrames
    if($LASTEXITCODE -ne 0) {throw "State snapshot failed: $LASTEXITCODE"}
    if($Label -eq 'candidate') {
        $vmadBase=Join-Path $vmadBuild "vmad-before-$vmadFrames.bin"
        $vmadExpected=(Get-FileHash -Algorithm SHA256 -LiteralPath $vmadBase).Hash
        if((Get-FileHash -Algorithm SHA256 -LiteralPath $vmadState).Hash -ne $vmadExpected) {throw "Complete scalar/SIMD state differs after $vmadFrames fields"}
        "Matching scalar/SIMD state after $vmadFrames fields" | Out-Host
    }
}
