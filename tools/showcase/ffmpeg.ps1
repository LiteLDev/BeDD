# ffmpeg for the showcase scripts: $env:FFMPEG, ffmpeg on PATH, or the winget package (installed
# here when missing).
function Find-FFmpeg {
    if ($env:FFMPEG) { return $env:FFMPEG }
    $c = Get-Command ffmpeg -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($c) { return $c.Source }
    $find = { Get-ChildItem (Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages\Gyan.FFmpeg_*\*\bin\ffmpeg.exe') -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime | Select-Object -Last 1 -ExpandProperty FullName }
    $exe = & $find
    if ($exe) { return $exe }
    Write-Host '[showcase] installing ffmpeg (winget Gyan.FFmpeg)'
    winget install --id Gyan.FFmpeg -e --silent --accept-source-agreements --accept-package-agreements | Out-Host
    $exe = & $find
    if (-not $exe) { throw 'ffmpeg not found: install it (winget install Gyan.FFmpeg) or set $env:FFMPEG' }
    return $exe
}
