#!/usr/bin/env node
// Puts a showcase take (record.mjs) together into the files in docs/public/showcase/.
//
// A take is two recordings: the manager window (out/take-<n>-manager.mkv) and the terminal pane
// (out/take-<n>-terminal.mkv, demo.mjs's output). The pictures put them side by side, 1920x1080: the
// manager on the left, the terminal on the right (on the console's black where it is shorter than
// the picture). script-and-game is one game's tile from the manager (--tile, in manager pixels) next
// to the top of the terminal.
//
//   node encode.mjs --take 2 --start 1.5 --duration 11 --still 6

import { parseArgs } from 'node:util'
import { readdirSync, statSync, existsSync } from 'node:fs'
import { join, dirname } from 'node:path'
import { fileURLToPath } from 'node:url'
import { findFFmpeg, ff, videoSize } from './ffmpeg.mjs'

const here = dirname(fileURLToPath(import.meta.url))

const HELP = `usage: node encode.mjs --take <n> [--start 0] [--duration 11] [--still 5] [--side-still 6]
                        [--width 1920] [--height 1080] [--tile 606:381:12:572] [--dry-run]

  --take <n>          the take to encode (out/take-<n>-manager.mkv + take-<n>-terminal.mkv)
  --start <s>         where the cut starts in the take
  --duration <s>      length of the cut
  --still <s>         seconds into the cut for the desktop poster
  --side-still <s>    seconds into the cut for the script-and-game poster
  --tile <w:h:x:y>    game 3's tile (header included) in the manager recording (the window
                      scaled to fit 1248x1080: measure it on a frame of the take)
  --dry-run           print the ffmpeg command lines`

const { values } = parseArgs({
  options: {
    take: { type: 'string' },
    start: { type: 'string', default: '0' },
    duration: { type: 'string', default: '11' },
    still: { type: 'string', default: '5' },
    'side-still': { type: 'string', default: '6' },
    width: { type: 'string', default: '1920' },
    height: { type: 'string', default: '1080' },
    tile: { type: 'string', default: '606:381:12:572' },
    'dry-run': { type: 'boolean' },
    help: { type: 'boolean', short: 'h' }
  }
})
if (values.help || !values.take) {
  console.log(HELP)
  process.exit(values.help ? 0 : 2)
}

const num = name => {
  const v = Number(values[name])
  if (!Number.isFinite(v)) {
    console.error(`error: --${name} must be a number`)
    process.exit(2)
  }
  return v
}
const take = num('take')
const start = num('start')
const duration = num('duration')
const still = num('still')
const sideStill = num('side-still')
const width = num('width')
const height = num('height')
const [tw, th, tx, ty] = values.tile.split(':')
const dry = values['dry-run']

const manager = join(here, 'out', `take-${take}-manager.mkv`)
const terminal = join(here, 'out', `take-${take}-terminal.mkv`)
const dst = join(here, '..', '..', 'docs', 'public', 'showcase')
const exe = dry ? (findFFmpeg({ install: false }) ?? 'ffmpeg') : findFFmpeg()
const run = args => dry ? console.log(['ffmpeg', ...args].map(a => /[\s;[\]]/.test(a) ? `"${a}"` : a).join(' ')) : ff(exe, args)

if (!dry) {
  for (const f of [manager, terminal]) {
    if (!existsSync(f)) {
      console.error(`error: ${f} not found (record a take first: node record.mjs)`)
      process.exit(4)
    }
  }
}
const termW = width - (dry && !existsSync(manager) ? 1248 : videoSize(exe, manager).width)
const mp4 = ['-an', '-c:v', 'libx264', '-preset', 'slow', '-pix_fmt', 'yuv420p', '-movflags', '+faststart']

// The two recordings cut at the same moment (at, len) and put together: [d] the picture,
// [s] script-and-game; pick of them goes through filter to the output.
function compose (at, len, pick, filter) {
  const graph =
    // The terminal holds its last frame to the end of the cut.
    `[1:v]tpad=stop_mode=clone:stop_duration=${len},crop=min(iw\\,${termW}):min(ih\\,${height}):0:0,` +
      `pad=${termW}:${height}:0:0:color=0x0c0c0c,split[t1][t2];` +
    `[0:v]crop=iw:min(ih\\,${height}):0:0,pad=iw:${height}:0:0:color=0x1b1b1f,split[m1][m2];` +
    '[m1][t1]hstack,setsar=1[d];' +
    `[m2]crop=${tw}:${th}:${tx}:${ty},scale=-2:540:flags=lanczos[g];[t2]crop=iw:540:0:0[tt];[g][tt]hstack,setsar=1[s];` +
    `[${pick}]${filter}[o];[${pick === 'd' ? 's' : 'd'}]nullsink`
  return ['-ss', String(at), '-t', String(len), '-i', manager, '-ss', String(at), '-i', terminal,
    '-filter_complex', graph, '-map', '[o]', '-t', String(len)]
}

// The whole picture at 1280x720, its poster, and the README's animated WebP (GitHub cannot play
// repo videos inline).
run([...compose(start, duration, 'd', 'scale=1280:720:flags=lanczos,fps=30'), ...mp4, '-crf', '26', join(dst, 'desktop.mp4')])
run([...compose(start + still, 1, 'd', 'scale=1280:720:flags=lanczos'), '-frames:v', '1', '-c:v', 'libwebp', '-quality', '85', join(dst, 'desktop-poster.webp')])
run([...compose(start, duration, 'd', 'scale=960:540:flags=lanczos,fps=10'), '-c:v', 'libwebp_anim', '-quality', '50', '-compression_level', '6', '-loop', '0', join(dst, 'desktop.webp')])
// One game and the script side by side.
run([...compose(start, duration, 's', 'scale=-2:360:flags=lanczos,fps=30'), ...mp4, '-crf', '24', join(dst, 'script-and-game.mp4')])
run([...compose(start + sideStill, 1, 's', 'scale=-2:360:flags=lanczos'), '-frames:v', '1', '-c:v', 'libwebp', '-quality', '85', join(dst, 'script-and-game-poster.webp')])

if (!dry) {
  for (const name of readdirSync(dst)) console.log(`${name.padEnd(32)} ${Math.round(statSync(join(dst, name)).size / 1024)} KB`)
}
