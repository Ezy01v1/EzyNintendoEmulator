param(
    [Parameter(Mandatory=$true)][string]$Rom,
    [string]$Runner,
    [string]$BaselineRunner,
    [ValidateSet('interpreter','jit')][string]$Backend='interpreter',
    [ValidateRange(1,20)][int]$Pairs=3,
    [ValidateRange(0,1000000)][int]$First=1500,
    [ValidateRange(1,1000000)][int]$End=1900,
    [string]$OutputDirectory
)
$ErrorActionPreference='Stop'
$raceRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if(!$Runner){$Runner=Join-Path $raceRoot 'build/default/tools/n64_runner/Release/ezy_n64_runner.exe'}
if(!$OutputDirectory){$OutputDirectory=Join-Path $raceRoot 'build/race-benchmark'}
if($End -le $First){throw 'End must be greater than First'}
foreach($racePath in @($Rom,$Runner)) {
    if(!(Test-Path -LiteralPath $racePath -PathType Leaf)){throw "Missing input: $racePath"}
}
if($BaselineRunner -and !(Test-Path -LiteralPath $BaselineRunner -PathType Leaf)){throw 'Missing baseline runner'}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$raceCases=if($BaselineRunner){
    @([pscustomobject]@{Label='baseline';Exe=$BaselineRunner;Backend=$Backend},
      [pscustomobject]@{Label='candidate';Exe=$Runner;Backend=$Backend})
}else{
    @([pscustomobject]@{Label='interpreter';Exe=$Runner;Backend='interpreter'},
      [pscustomobject]@{Label='jit';Exe=$Runner;Backend='jit'})
}
$raceRows=@()
for($racePair=0;$racePair -lt $Pairs;++$racePair){
    $raceOrder=if($racePair%2 -eq 0){@(0,1)}else{@(1,0)}
    foreach($raceIndex in $raceOrder){
        $raceCase=$raceCases[$raceIndex]
        $raceStem=Join-Path $OutputDirectory "$($raceCase.Label)-$racePair"
        "Running pair $racePair, $($raceCase.Label), fields $First..$End" | Out-Host
        $raceLog=@(& $raceCase.Exe $Rom "--rsp-$($raceCase.Backend)" --rdp-sync --benchmark "$raceStem.bmp" $End --time-from $First --dump-final-state "$raceStem.bin")
        if($LASTEXITCODE -ne 0){throw "Benchmark failed: $raceStem"}
        $raceLog | Set-Content -LiteralPath "$raceStem.log"
        $raceMeasurements=@($raceLog | Where-Object {$_ -like 'Benchmark: *'})
        if($raceMeasurements.Count -ne 1){throw 'Missing or ambiguous benchmark result'}
        $raceMeasurement=$raceMeasurements[0].Substring(11) | ConvertFrom-Json
        if($raceMeasurement.profiled -or $raceMeasurement.first -ne $First -or $raceMeasurement.end -ne $End -or
           $raceMeasurement.fields -ne ($End-$First) -or $raceMeasurement.wall_seconds -le 0 -or
           $raceMeasurement.cpu_seconds -lt 0){throw 'Invalid measurement window or timers'}
        $raceRow=[pscustomobject]@{
            Pair=$racePair; Variant=$raceCase.Label; Backend=$raceCase.Backend
            First=$First; End=$End; WallSeconds=$raceMeasurement.wall_seconds
            CpuSeconds=$raceMeasurement.cpu_seconds; FieldsPerSecond=$raceMeasurement.fields_per_second
            ImageHash=(Get-FileHash -LiteralPath "$raceStem.bmp" -Algorithm SHA256).Hash
            StateHash=(Get-FileHash -LiteralPath "$raceStem.bin" -Algorithm SHA256).Hash
        }
        $raceRows+=$raceRow
        $raceRows | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'results.json')
        $raceRow | ConvertTo-Json -Compress | Out-Host
        if(@($raceRows.ImageHash | Sort-Object -Unique).Count -ne 1 -or
           @($raceRows.StateHash | Sort-Object -Unique).Count -ne 1){throw 'Complete game states or images differ'}
    }
}
'All game states and images match exactly' | Out-Host
