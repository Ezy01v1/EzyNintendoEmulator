param([ValidateSet('before','candidate')][string]$Label='before')
$ErrorActionPreference='Stop'
$vmadRoot=[System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$vmadBuild=Join-Path $vmadRoot 'build'
$vmadExe=Join-Path $vmadBuild 'default/tools/n64_runner/Release/ezy_rsp_vector_bench.exe'
$vmadPath=Join-Path $vmadBuild "vmad-$Label.csv"
& $vmadExe 1000000 | Set-Content -LiteralPath $vmadPath
if($LASTEXITCODE -ne 0) {throw "Microbenchmark failed: $LASTEXITCODE"}
$vmadRows=@(Import-Csv -LiteralPath $vmadPath)
if($Label -eq 'candidate') {
    $vmadReference=@(Import-Csv -LiteralPath (Join-Path $vmadBuild 'vmad-before.csv'))
    if($vmadRows.Count -ne $vmadReference.Count) {throw 'Scalar/SIMD benchmark row counts differ'}
    for($vmadIndex=0;$vmadIndex -lt $vmadRows.Count;++$vmadIndex) {
        foreach($vmadField in @('Opcode','Pattern','Repeat','Instructions','Checksum')) {
            if($vmadRows[$vmadIndex].$vmadField -ne $vmadReference[$vmadIndex].$vmadField) {
                throw "Scalar/SIMD benchmark differs at row $vmadIndex ($vmadField)"
            }
        }
    }
    'Matching scalar/SIMD checksums for all benchmark rows' | Out-Host
}
$vmadRows | Group-Object Opcode,Pattern | ForEach-Object {
    $vmadTimes=@($_.Group.WallNsPerInstruction | ForEach-Object {[double]$_} | Sort-Object)
    '{0}: median {1:F3} ns/instruction, checksum variants {2}' -f $_.Name,$vmadTimes[2],@($_.Group.Checksum | Sort-Object -Unique).Count
} | Out-Host
