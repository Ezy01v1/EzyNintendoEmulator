param([ValidateSet('before','candidate')][string]$Label='before')
$ErrorActionPreference='Stop'
$vmadRoot=[System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$vmadBuild=Join-Path $vmadRoot 'build'
$vmadExe=Join-Path $vmadBuild 'default/tools/n64_runner/Release/ezy_rsp_vector_bench.exe'
$vmadPath=Join-Path $vmadBuild "vmad-$Label.csv"
& $vmadExe 1000000 | Set-Content -LiteralPath $vmadPath
if($LASTEXITCODE -ne 0) {throw "Microbenchmark failed: $LASTEXITCODE"}
Import-Csv -LiteralPath $vmadPath | Group-Object Opcode,Pattern | ForEach-Object {
    $vmadTimes=@($_.Group.WallNsPerInstruction | ForEach-Object {[double]$_} | Sort-Object)
    '{0}: median {1:F3} ns/instruction, checksum variants {2}' -f $_.Name,$vmadTimes[2],@($_.Group.Checksum | Sort-Object -Unique).Count
} | Out-Host
