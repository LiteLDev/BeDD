<#
.SYNOPSIS
  Cuts a showcase take (record.ps1) into the files in docs\public\showcase\.

.EXAMPLE
  pwsh -NoProfile -File tools/showcase/encode.ps1 -Take 2 -Start 1.5 -Duration 11 -Still 6
#>
param(
    [Parameter(Mandatory)][int]$Take,
    [double]$Start = 0,
    [double]$Duration = 11,
    [double]$Still = 5,          # seconds into the cut for the still picture
    [double]$SideStill = 6       # seconds into the cut for the script-and-game poster
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ffmpeg.ps1')
$FFmpeg = Find-FFmpeg
$src = Join-Path $PSScriptRoot "out\take-$Take.mkv"
$dst = (Resolve-Path (Join-Path $PSScriptRoot '..\..\docs\public\showcase')).Path
function ff { & $FFmpeg -hide_banner -loglevel error -y @args; if ($LASTEXITCODE) { throw "ffmpeg failed: $args" } }
$cut = @('-ss', $Start, '-t', $Duration, '-i', $src)
$mp4 = @('-an', '-c:v', 'libx264', '-preset', 'slow', '-pix_fmt', 'yuv420p', '-movflags', '+faststart')

# The whole desktop region (1920x1080 physical pixels) at 1280x720.
ff @cut -vf 'scale=1280:720:flags=lanczos,fps=30' @mp4 -crf 28 (Join-Path $dst 'desktop.mp4')
ff -ss ($Start + $Still) -i $src '-frames:v' 1 -vf 'scale=1280:720:flags=lanczos' '-c:v' libwebp -quality 85 (Join-Path $dst 'desktop-poster.webp')
# README hero: animated WebP (GitHub cannot play repo videos inline).
ff @cut -vf 'scale=960:540:flags=lanczos,fps=10' '-c:v' libwebp_anim -quality 50 -compression_level 6 -loop 0 (Join-Path $dst 'desktop.webp')
# The script and game 3 side by side: the bottom row of the grid.
ff @cut -vf 'crop=1920:540:0:540,scale=1280:360:flags=lanczos,fps=30' @mp4 -crf 26 (Join-Path $dst 'script-and-game.mp4')
ff -ss ($Start + $SideStill) -i $src '-frames:v' 1 -vf 'crop=1920:540:0:540,scale=1280:360:flags=lanczos' '-c:v' libwebp -quality 85 (Join-Path $dst 'script-and-game-poster.webp')

Get-ChildItem $dst | Select-Object Name, @{ n = 'KB'; e = { [int]($_.Length / 1KB) } } | Format-Table -AutoSize
