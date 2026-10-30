<#
.SYNOPSIS
  Puts a showcase take (record.ps1) together into the files in docs\public\showcase\.

.DESCRIPTION
  A take is two recordings: the manager window (take-<n>-manager.mkv) and the terminal running the
  script (take-<n>-terminal.mkv). The pictures put them side by side, 1920x1080: the manager on the
  left, the terminal on the right (on the console's black where it is shorter than the picture).
  script-and-game is one game's tile from the manager (-Tile, in manager pixels) next to the top of
  the terminal.

.EXAMPLE
  pwsh -NoProfile -File tools/showcase/encode.ps1 -Take 2 -Start 1.5 -Duration 11 -Still 6
#>
param(
    [Parameter(Mandatory)][int]$Take,
    [double]$Start = 0,
    [double]$Duration = 11,
    [double]$Still = 5,                  # seconds into the cut for the still picture
    [double]$SideStill = 6,              # seconds into the cut for the script-and-game poster
    [int]$Width = 1920, [int]$Height = 1080,
    [string]$Tile = '606:381:12:572'     # w:h:x:y of game 3's tile (header included) in the manager recording
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ffmpeg.ps1')
$FFmpeg = Find-FFmpeg
$manager = Join-Path $PSScriptRoot "out\take-$Take-manager.mkv"
$terminal = Join-Path $PSScriptRoot "out\take-$Take-terminal.mkv"
$dst = (Resolve-Path (Join-Path $PSScriptRoot '..\..\docs\public\showcase')).Path
function ff { & $FFmpeg -hide_banner -loglevel error -y @args; if ($LASTEXITCODE) { throw "ffmpeg failed: $args" } }
$mp4 = @('-an', '-c:v', 'libx264', '-preset', 'slow', '-pix_fmt', 'yuv420p', '-movflags', '+faststart')

function Get-Width([string]$File) {
    $info = & $FFmpeg -hide_banner -i $File 2>&1 | Select-String 'Video: .*?, (\d+)x(\d+)' | Select-Object -First 1
    [int]$info.Matches[0].Groups[1].Value
}
$termW = $Width - (Get-Width $manager)
$tw, $th, $tx, $ty = $Tile -split ':'

# The two recordings cut at the same moment ($At, $Len) and put together: [d] the picture, [s] script-and-game;
# $Pick of them goes through $Filter to the output.
function Compose([double]$At, [double]$Len, [ValidateSet('d', 's')][string]$Pick, [string]$Filter) {
    # A window capture has frames only while the window changes: the terminal holds its last one.
    $graph = "[1:v]tpad=stop_mode=clone:stop_duration=$Len,crop=min(iw\,$termW):min(ih\,$Height):0:0,pad=${termW}:${Height}:0:0:color=0x0c0c0c,split[t1][t2];" +
        "[0:v]crop=iw:min(ih\,$Height):0:0,pad=iw:${Height}:0:0:color=0x1b1b1f,split[m1][m2];" +
        "[m1][t1]hstack,setsar=1[d];" +
        "[m2]crop=${tw}:${th}:${tx}:${ty},scale=-2:540:flags=lanczos[g];[t2]crop=iw:540:0:0[tt];[g][tt]hstack,setsar=1[s];" +
        "[$Pick]$Filter[o];[$(if ($Pick -eq 'd') { 's' } else { 'd' })]nullsink"
    return @('-ss', $At, '-t', $Len, '-i', $manager, '-ss', $At, '-i', $terminal, '-filter_complex', $graph, '-map', '[o]', '-t', $Len)
}

# The whole picture at 1280x720, its poster, and the README's animated WebP (GitHub cannot play repo videos inline).
$a = Compose $Start $Duration d 'scale=1280:720:flags=lanczos,fps=30'
ff @a @mp4 -crf 26 (Join-Path $dst 'desktop.mp4')
$a = Compose ($Start + $Still) 1 d 'scale=1280:720:flags=lanczos'
ff @a '-frames:v' 1 '-c:v' libwebp -quality 85 (Join-Path $dst 'desktop-poster.webp')
$a = Compose $Start $Duration d 'scale=960:540:flags=lanczos,fps=10'
ff @a '-c:v' libwebp_anim -quality 50 -compression_level 6 -loop 0 (Join-Path $dst 'desktop.webp')
# One game and the script side by side.
$a = Compose $Start $Duration s 'scale=-2:360:flags=lanczos,fps=30'
ff @a @mp4 -crf 24 (Join-Path $dst 'script-and-game.mp4')
$a = Compose ($Start + $SideStill) 1 s 'scale=-2:360:flags=lanczos'
ff @a '-frames:v' 1 '-c:v' libwebp -quality 85 (Join-Path $dst 'script-and-game-poster.webp')

Get-ChildItem $dst | Select-Object Name, @{ n = 'KB'; e = { [int]($_.Length / 1KB) } } | Format-Table -AutoSize
