# The script the showcase video shows in its terminal (record.ps1 opens it in a console window).
# Each game gets its own runspace and its own pipe; every line printed is exactly the command run.
param([Parameter(Mandatory)][string]$Pipes, [string]$GoFile, [int]$LingerSeconds = 6)
$ErrorActionPreference = 'Stop'

$module = Join-Path (Resolve-Path "$PSScriptRoot/../..").Path 'bin\BeDD\BeDD.psm1'
Import-Module $module   # once here, so the runspaces below find its types already loaded
$pipeList = $Pipes -split ','

$Host.UI.RawUI.WindowTitle = 'e2e - my-client-mod'
# Nothing printed before the take starts: the window gets its final size meanwhile.
if ($GoFile) { while (-not (Test-Path $GoFile)) { Start-Sleep -Milliseconds 50 } }
Clear-Host
Write-Host 'PS C:\src\my-client-mod> ' -NoNewline -ForegroundColor DarkGray
Write-Host '.\e2e.ps1 -Games 3'
Write-Host "[1] [2] [3]: three headless games, one pipe each`n" -ForegroundColor DarkGray

$scripts = @{
    1 = @(
        300, '1..40 | % { Send-BeDDRaw move 12; sleep -m 20 }',
        300, 'Send-BeDDKey SPACE',
        700, '1..40 | % { Send-BeDDRaw move -9; sleep -m 20 }'
    )
    2 = @(
        900, 'Send-BeDDKey T',
        500, "Send-BeDDText '/summon sheep ^ ^ ^3' -DelayMs 40",
        200, 'Send-BeDDKey ENTER',
        1200, 'Send-BeDDKey T',
        500, "Send-BeDDText '/summon cow ^2 ^ ^4' -DelayMs 40",
        200, 'Send-BeDDKey ENTER'
    )
    3 = @(
        1800, 'Send-BeDDKey E',
        900, 'Send-BeDDMouse move -RX 0.504 -RY 0.644',
        1400, 'Send-BeDDMouse click',
        600, 'Send-BeDDMouse click -RX 0.588 -RY 0.562',
        900, 'Send-BeDDMouse move -RX 0.61 -RY 0.644',
        1600, 'Send-BeDDKey ESC'
    )
}
$colors = @{ 1 = 'Cyan'; 2 = 'Yellow'; 3 = 'Green' }

1..$pipeList.Count | ForEach-Object -ThrottleLimit 8 -Parallel {
    $n = $_
    Import-Module $using:module
    Connect-BeDD -PipeName ($using:pipeList)[$n - 1] -TimeoutSeconds 10 | Out-Null
    $steps = ($using:scripts)[$n]
    $color = ($using:colors)[$n]
    for ($i = 0; $i -lt $steps.Count; $i += 2) {
        Start-Sleep -Milliseconds $steps[$i]
        Write-Host "[$n] " -NoNewline -ForegroundColor $color
        Write-Host $steps[$i + 1]
        Invoke-Expression $steps[$i + 1]
    }
}

Write-Host ''
Write-Host 'Done: 3 games driven, your mouse and keyboard untouched.' -ForegroundColor Green
Start-Sleep -Seconds $LingerSeconds
