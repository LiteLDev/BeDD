// ffmpeg for the showcase scripts: $FFMPEG, ffmpeg on PATH, or the winget package (installed here
// when missing). The recording needs ffmpeg 8 or newer (gfxcapture) built with libass (the
// terminal pane) and libwebp: the "full" build of Gyan.FFmpeg has all of them.

import { existsSync, readdirSync, statSync } from 'node:fs'
import { join, delimiter } from 'node:path'
import { spawnSync } from 'node:child_process'

function fromPath () {
  for (const dir of (process.env.PATH ?? '').split(delimiter)) {
    if (!dir) continue
    const exe = join(dir.replace(/^"|"$/g, ''), 'ffmpeg.exe')
    if (existsSync(exe)) return exe
  }
  return null
}

function fromWinget () {
  const root = join(process.env.LOCALAPPDATA ?? '', 'Microsoft', 'WinGet', 'Packages')
  let best = null
  let bestTime = 0
  let packages
  try {
    packages = readdirSync(root).filter(n => n.startsWith('Gyan.FFmpeg_'))
  } catch {
    return null
  }
  for (const pkg of packages) {
    let builds
    try { builds = readdirSync(join(root, pkg)) } catch { continue }
    for (const build of builds) {
      const exe = join(root, pkg, build, 'bin', 'ffmpeg.exe')
      if (!existsSync(exe)) continue
      const t = statSync(exe).mtimeMs
      if (t > bestTime) { best = exe; bestTime = t }
    }
  }
  return best
}

export function findFFmpeg ({ install = true } = {}) {
  if (process.env.FFMPEG) return process.env.FFMPEG
  const found = fromPath() ?? fromWinget()
  if (found || !install) return found
  console.error('[showcase] installing ffmpeg (winget Gyan.FFmpeg)')
  spawnSync('winget', ['install', '--id', 'Gyan.FFmpeg', '-e', '--silent',
    '--accept-source-agreements', '--accept-package-agreements'], { stdio: 'inherit' })
  const exe = fromWinget()
  if (!exe) throw new Error('ffmpeg not found: install it (winget install Gyan.FFmpeg) or set FFMPEG')
  return exe
}

// Runs ffmpeg to completion (quiet, overwriting); throws on a non-zero exit.
export function ff (exe, args, { cwd } = {}) {
  const r = spawnSync(exe, ['-hide_banner', '-loglevel', 'error', '-y', ...args], { stdio: ['ignore', 'inherit', 'inherit'], cwd })
  if (r.error) throw r.error
  if (r.status !== 0) throw new Error(`ffmpeg failed (${r.status}): ${args.join(' ')}`)
}

// Width and height of the first video stream of a file.
export function videoSize (exe, file) {
  const r = spawnSync(exe, ['-hide_banner', '-i', file], { encoding: 'utf8' })
  const m = /Video: .*?, (\d+)x(\d+)/.exec(r.stderr ?? '')
  if (!m) throw new Error(`no video stream in ${file}`)
  return { width: Number(m[1]), height: Number(m[2]) }
}
