[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$BuildDirectory,
    [string]$LogDirectory='logs/audio-continuity-repair-20260915',
    [switch]$DriftOnly
)
$ErrorActionPreference='Stop'
$audioBin=(Resolve-Path -LiteralPath $BuildDirectory).Path
$audioLogs=[IO.Path]::GetFullPath($LogDirectory)
[IO.Directory]::CreateDirectory($audioLogs) | Out-Null
$cases=@(
    @{Name='waveform';Exe='veyra_audio_waveform_tests.exe';Args=@('--offline');Seconds=30},
    @{Name='wasapi-clock';Exe='veyra_wasapi_input_tests.exe';Args=@('--offline');Seconds=30},
    @{Name='queued-empty-pull';Exe='veyra_audio_waveform_tests.exe';Args=@('--endpoint');Seconds=30},
    @{Name='true-endpoint-gap';Exe='veyra_audio_waveform_tests.exe';Args=@('--endpoint-gap');Seconds=30},
    @{Name='capture-baseline';Exe='veyra_capture_audio_tests.exe';Args=@('--baseline');Seconds=90},
    @{Name='capture-clock-audit';Exe='veyra_capture_audio_tests.exe';Args=@('--sync-clock-audit');Seconds=30},
    @{Name='legacy-clock';Exe='veyra_capture_audio_tests.exe';Args=@('--legacy-clock');Seconds=90},
    @{Name='jitter-stereo';Exe='veyra_capture_audio_tests.exe';Args=@('--jitter');Seconds=30},
    @{Name='slow-start';Exe='veyra_capture_audio_tests.exe';Args=@('--slow-start');Seconds=30},
    @{Name='jitter-51';Exe='veyra_capture_audio_tests.exe';Args=@('--jitter','--5.1');Seconds=30},
    @{Name='transient';Exe='veyra_capture_audio_tests.exe';Args=@('--transient');Seconds=30},
    @{Name='transient-comp';Exe='veyra_capture_audio_tests.exe';Args=@('--transient-comp');Seconds=30},
    @{Name='endpoint-loss';Exe='veyra_capture_audio_tests.exe';Args=@('--endpoint-loss');Seconds=30},
    @{Name='multichannel';Exe='veyra_multichannel_tests.exe';Args=@("$audioLogs/multichannel-fixtures");Seconds=60},
    @{Name='timeline';Exe='veyra_audio_timeline_tests.exe';Args=@("$audioLogs/timeline-fixtures");Seconds=120},
    @{Name='timeline-jitter';Exe='veyra_audio_timeline_tests.exe';Args=@("$audioLogs/timeline-jitter-fixtures",'--jitter');Seconds=60}
)
if($DriftOnly){$cases=@(
    @{Name='drift-fast';Exe='veyra_capture_audio_tests.exe';Args=@('--drift-fast');Seconds=150},
    @{Name='drift-slow';Exe='veyra_capture_audio_tests.exe';Args=@('--drift-slow');Seconds=150}
)}
$results=@()
foreach($case in $cases){
    Write-Host ('START '+$case.Name)
    $quoted=@($case.Args | ForEach-Object {'"'+$_.Replace('"','\"')+'"'})
    $process=Start-Process -FilePath (Join-Path $audioBin $case.Exe) -ArgumentList $quoted -PassThru -WindowStyle Hidden -RedirectStandardOutput "$audioLogs/$($case.Name).stdout.log" -RedirectStandardError "$audioLogs/$($case.Name).stderr.log"
    $processHandle=$process.Handle
    if(-not $process.WaitForExit($case.Seconds*1000)){
        Stop-Process -Id $process.Id -Force
        $code=124
    }else{$process.Refresh();$code=$process.ExitCode}
    $results+=@{name=$case.Name;exitCode=$code;timeoutSeconds=$case.Seconds}
    Write-Host ('END '+$case.Name+' exit='+$code)
}
$name=if($DriftOnly){'drift-results.json'}else{'results.json'}
$results | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $audioLogs $name) -Encoding UTF8
if(@($results | Where-Object {$null -eq $_.exitCode -or $_.exitCode -ne 0}).Count){exit 1}
exit 0
