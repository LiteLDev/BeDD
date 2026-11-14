#!/usr/bin/env node
// Records the showcase video: three headless games driven from a script, as the manager window
// shows them.
//
// Everything goes through BeDD 0.4: the app CLI (BeDD.exe) clones the instances, installs the mod,
// makes the world single-player, launches and stops the games (it also holds the slot locks); every
// in-game action is a request on the game's WebSocket (lib/bedd.mjs). bedboo ops first (`look`,
// `input`, `send` for commands, `world` to know where the game is), the `ui` tree to find buttons by
// name, and the plain mouse/key ops only where the screen has nothing semantic to offer (OreUI
// screens are one opaque node in the tree); each of those says why.
//
// A take: the scene is staged per game (scene.mjs, read again every take), ffmpeg records the
// manager window with gfxcapture (Windows.Graphics.Capture: the window's own picture, whatever
// covers it, found by its title and exe, nothing moved or focused) while demo.mjs drives the games
// and writes its transcript, which is then drawn into the terminal pane (lib/terminal.mjs).
// Output: out/take-<n>-manager.mkv, out/take-<n>-terminal.mkv (near-lossless), setup screenshots;
// encode.mjs puts a take together into docs/public/showcase/.
//
// --keep-open keeps the games running after the first take: create out/take for another take
// (scene.mjs and demo.mjs are re-read), out/stop to end. Cleanup always stops the games it launched
// and restores the options it changed. --dry-run prints the planned CLI calls, file changes, ffmpeg
// command lines and WebSocket requests without running anything.
//
//   node record.mjs --keep-open
//   node record.mjs --dry-run

import { parseArgs } from 'node:util'
import { spawn, spawnSync } from 'node:child_process'
import {
  existsSync, mkdirSync, readdirSync, readFileSync, writeFileSync, renameSync, rmSync
} from 'node:fs'
import { join, dirname, delimiter } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'
import { connect, listEndpoints } from './lib/bedd.mjs'
import { click, pickRef, SOFT } from './lib/ui.mjs'
import { renderAss } from './lib/terminal.mjs'
import { dryClient } from './lib/dry.mjs'
import { findFFmpeg, ff } from './ffmpeg.mjs'

const here = dirname(fileURLToPath(import.meta.url))
const Root = join(here, '..', '..')
const Out = join(here, 'out')
const WorldZip = join(here, 'fixtures', 'Showcase.zip')
const WorldName = 'LHI-Showcase'
// Game client size (16:9, as the manager window shows it), the picture (physical pixels) and the
// manager's share of its width (the terminal pane gets the rest).
const ClientW = 848; const ClientH = 477
const RegionW = 1920; const RegionH = 1080
const ManagerW = 1248
// World list tiles (fractions of the client area). The first tile can be a featured server shown as
// a "LAN world" (it sorts before local worlds): then the test world is the next tile.
const TileX = [0.173, 0.383, 0.593, 0.803]
const TileY = 0.43

const HELP = `usage: node record.mjs [--slots 2,3,4] [--base <instance>] [--take-seconds 24] [--keep-open]
                        [--max-minutes 60] [--bedd <BeDD.exe>] [--manager-title <regex>] [--dry-run]

  --slots <list>          clone slots of the three games (instances <base>-<slot>)
  --base <name>           LeviLauncher instance to clone (default: the one BeDD.exe picks)
  --take-seconds <s>      length of a take
  --keep-open             keep the games for more takes (out/take: another take, out/stop: end)
  --max-minutes <m>       end a --keep-open session after this long
  --bedd <path>           BeDD.exe (default: $BEDD_EXE, bin/BeDD/BeDD.exe, the app's publish folder, PATH)
  --manager-title <re>    the manager window's title, an ECMAScript regex for gfxcapture
                          (no : , ; [ ] ' or \\ in it; default "(?i)^BeDD")
  --no-manager            do not start the manager window (one is already open)
  --dry-run               print the plan (CLI calls, files, ffmpeg, WebSocket requests); run nothing
  -h, --help              this text`

const { values } = parseArgs({
  options: {
    slots: { type: 'string', default: '2,3,4' },
    base: { type: 'string' },
    'take-seconds': { type: 'string', default: '24' },
    'keep-open': { type: 'boolean' },
    'max-minutes': { type: 'string', default: '60' },
    bedd: { type: 'string' },
    'manager-title': { type: 'string', default: '(?i)^BeDD' },
    'no-manager': { type: 'boolean' },
    'dry-run': { type: 'boolean' },
    help: { type: 'boolean', short: 'h' }
  }
})
if (values.help) {
  console.log(HELP)
  process.exit(0)
}
const dry = values['dry-run']
const slots = values.slots.split(',').map(Number)
const takeSeconds = Number(values['take-seconds'])
const maxMinutes = Number(values['max-minutes'])
if (slots.length !== 3 || slots.some(s => !Number.isInteger(s) || s < 1) || !(takeSeconds > 0) || !(maxMinutes > 0)) {
  console.error('error: --slots takes three slot numbers, --take-seconds and --max-minutes positive numbers')
  process.exit(2)
}
if (/[:,;[\]'\\]/.test(values['manager-title'])) {
  console.error('error: --manager-title cannot contain : , ; [ ] \' or \\ (ffmpeg filter syntax)')
  process.exit(2)
}

const log = m => console.log(`[showcase] ${m}`)
const plan = m => console.log(`  ${m}`)
const sleep = ms => dry ? Promise.resolve() : new Promise(resolve => setTimeout(resolve, ms))
const q = a => /[\s"]/.test(a) ? `"${a.replace(/"/g, '\\"')}"` : a

// --- the app CLI -------------------------------------------------------------------------------

function findBeDD () {
  if (values.bedd) return values.bedd
  if (process.env.BEDD_EXE) return process.env.BEDD_EXE
  const candidates = [
    join(Root, 'bin', 'BeDD', 'BeDD.exe'),
    join(Root, 'app', 'BeDD', 'bin', 'Release', 'net10.0-windows', 'win-x64', 'publish', 'BeDD.exe')
  ]
  for (const dir of (process.env.PATH ?? '').split(delimiter)) if (dir) candidates.push(join(dir.replace(/^"|"$/g, ''), 'BeDD.exe'))
  const hit = candidates.find(f => existsSync(f))
  if (hit) return hit
  if (dry) return 'BeDD.exe'
  throw new Error('BeDD.exe not found: publish the app (see docs/reference/building.md), pass --bedd or set BEDD_EXE')
}
const BeDD = findBeDD()

// Plausible answers for --dry-run, shaped like the commands' --json output.
function dryAnswer (args) {
  const slot = Number(args[args.indexOf('--slot') + 1])
  const base = values.base ?? '<base>'
  const name = `${base}-${slot}`
  if (args[0] === 'instance') {
    return { name, dir: `%LOCALAPPDATA%\\BeDD\\instances\\${name}`, base, slot }
  }
  if (args[0] === 'launch') {
    return { pid: 0, url: 'ws://127.0.0.1:<port>/v1', token: '<token>', instance: name, label: args[args.indexOf('--label') + 1] }
  }
  return {}
}

// Runs BeDD.exe with args and returns its --json result. The CLI prints only the result on stdout,
// errors as {"error":{code,message,hint}} with a non-zero exit code.
function app (args) {
  plan(`> BeDD.exe ${args.map(q).join(' ')}`)
  if (dry) return dryAnswer(args)
  const r = spawnSync(BeDD, args, { encoding: 'utf8', windowsHide: true })
  if (r.error) throw r.error
  let doc = null
  try { doc = r.stdout.trim() ? JSON.parse(r.stdout) : null } catch {}
  if (r.status !== 0) {
    const e = doc?.error
    throw new Error(`BeDD.exe ${args.join(' ')}: ${e?.message || r.stderr.trim() || `exit ${r.status}`}` + (e?.hint ? ` (hint: ${e.hint})` : ''))
  }
  return doc ?? {}
}

// --- files -------------------------------------------------------------------------------------

function worldsDir (instanceDir) {
  const users = join(instanceDir, 'Minecraft Bedrock', 'Users')
  if (dry) return join(users, '<user>', 'games', 'com.mojang', 'minecraftWorlds')
  let dirs = []
  try {
    dirs = readdirSync(users, { withFileTypes: true }).filter(d => d.isDirectory())
      .map(d => ({ shared: d.name.toLowerCase() === 'shared', path: join(users, d.name, 'games', 'com.mojang', 'minecraftWorlds') }))
      .filter(d => existsSync(d.path))
  } catch {}
  return (dirs.find(d => !d.shared) ?? dirs[0])?.path ?? null
}

function optionsFile (instanceDir) {
  const users = join(instanceDir, 'Minecraft Bedrock', 'Users')
  if (dry) return join(users, '<user>', 'games', 'com.mojang', 'minecraftpe', 'options.txt')
  let names = []
  try { names = readdirSync(users) } catch {}
  const hits = names.map(n => ({ shared: n.toLowerCase() === 'shared', path: join(users, n, 'games', 'com.mojang', 'minecraftpe', 'options.txt') }))
    .filter(h => existsSync(h.path))
  return (hits.find(h => !h.shared) ?? hits[0])?.path ?? null
}

// Writes a value into a level.dat tag (Bedrock: 8-byte header, little-endian NBT).
function setLevelTag (worldDir, type, name, value) {
  for (const f of ['level.dat', 'level.dat_old']) {
    const path = join(worldDir, f)
    if (!existsSync(path)) continue
    const b = readFileSync(path)
    const nb = Buffer.from(name, 'utf8')
    const pat = Buffer.concat([Buffer.from([type, nb.length & 0xff, nb.length >> 8]), nb])
    const i = b.indexOf(pat, 8)
    if (i < 0) continue
    value.copy(b, i + pat.length)
    writeFileSync(path, b) // a file just unpacked from the zip, not a hardlink into the base instance
  }
}

// The Showcase world, fresh from the fixture, played last (first in the list) and without mobs
// spawning; made single-player by the app (no LAN / Xbox Live broadcast).
function restoreWorld (g) {
  const worlds = worldsDir(g.dir)
  if (!worlds) throw new Error(`no worlds folder in ${g.dir} (start the instance once)`)
  const w = join(worlds, WorldName)
  if (dry) {
    plan(`fs: replace ${w} with ${WorldZip} (tar -xf), levelname.txt "${WorldName}", level.dat LastPlayed=now domobspawning=0`)
  } else {
    for (const d of readdirSync(worlds, { withFileTypes: true })) {
      if (!d.isDirectory()) continue
      const nameFile = join(worlds, d.name, 'levelname.txt')
      if (existsSync(nameFile) && readFileSync(nameFile, 'utf8').trim() === WorldName) rmSync(join(worlds, d.name), { recursive: true, force: true })
    }
    mkdirSync(w, { recursive: true })
    // Windows' own bsdtar reads zip files (Git's GNU tar on PATH does not).
    const tar = join(process.env.SystemRoot ?? 'C:\\Windows', 'System32', 'tar.exe')
    const r = spawnSync(tar, ['-xf', WorldZip, '-C', w], { stdio: 'inherit' })
    if (r.status !== 0) throw new Error(`unpacking ${WorldZip} failed`)
    writeFileSync(join(w, 'levelname.txt'), WorldName)
    const now = Buffer.alloc(8); now.writeBigInt64LE(BigInt(Math.floor(Date.now() / 1000)))
    setLevelTag(w, 4, 'LastPlayed', now)
    setLevelTag(w, 1, 'domobspawning', Buffer.from([0]))
  }
  app(['world', 'singleplayer', w, '--json'])
}

// HUD on, GUI scale +1 for the picture. The file is replaced, never written in place (a clone's
// files can be hardlinks into the base instance): the original is renamed aside and renamed back.
function setOptions (g) {
  const file = optionsFile(g.dir)
  if (!file) return
  const aside = file + '.bedd-showcase'
  if (dry) {
    plan(`fs: ${file} -> ${aside}; new options.txt with gfx_hidehud:0 gfx_guiscale_offset:1`)
    g.options = file
    return
  }
  if (!existsSync(aside)) renameSync(file, aside) // else: the leftover of a killed run is the original
  else rmSync(file, { force: true })
  const text = readFileSync(aside, 'utf8')
    .replace(/^gfx_hidehud:.*$/m, 'gfx_hidehud:0')
    .replace(/^gfx_guiscale_offset:.*$/m, 'gfx_guiscale_offset:1')
  writeFileSync(file, text)
  g.options = file
}

function restoreOptions (g) {
  if (!g.options) return
  const aside = g.options + '.bedd-showcase'
  if (dry) { plan(`fs: ${aside} -> ${g.options}`); return }
  if (!existsSync(aside)) return
  rmSync(g.options, { force: true })
  renameSync(aside, g.options)
}

// --- driving -----------------------------------------------------------------------------------

const games = slots.map((slot, i) => ({ n: i + 1, slot, name: null, dir: null, pid: 0, endpoint: null, ws: null, options: null, launched: false }))

async function shot (g, name) {
  const file = join(Out, `g${g.n}-${name}.png`)
  // The game's own picture through PrintWindow, full size: works while the window is covered.
  const r = await g.ws.call('screenshot', { format: 'png', maxWidth: 0 })
  if (dry) plan(`fs: write ${file}`)
  else writeFileSync(file, Buffer.from(r.data, 'base64'))
}

const PLAY = /^play$/i
const CANCEL = /^cancel$/i

// The title screen is up when the tree shows its Play button, or (no tree) when `world` names the
// start screen. No pixel probing of screenshots.
async function atTitle (g) {
  try {
    const snap = await g.ws.call('ui', { format: 'json' })
    if (pickRef(snap.nodes, PLAY)) return true
  } catch (e) {
    if (!SOFT.has(e.code)) throw e
  }
  if (dry) return true
  const w = await g.ws.call('world')
  return !w.inLevel && /start/i.test(w.topScreen ?? '')
}

async function inWorld (g) {
  const w = await g.ws.call('world')
  return w.inLevel === true && w.player != null
}

async function enterWorld (g) {
  // Play is a button in the tree when the title screen is JSON UI; the point is the fallback.
  await click(g.ws, PLAY, { rx: 0.498, ry: 0.474 })
  await sleep(4000)
  await shot(g, 'worldlist')
  // The world list is an OreUI screen, one opaque node in the tree: only a point reaches a tile.
  await g.ws.call('mouse', { action: 'click', rx: TileX[0], ry: TileY })
}

// Waits until the game is in a world; clicks the world again when the first click did not open it
// (the world list sometimes takes longer to fill, or the first tile was a featured server).
async function waitInWorld (g) {
  for (let attempt = 1; attempt <= TileX.length; attempt++) {
    const deadline = Date.now() + 40000
    while (dry || Date.now() < deadline) {
      if (await inWorld(g)) { log(`game ${g.n}: in the world`); return }
      await sleep(1000)
    }
    await shot(g, `join-${attempt}`)
    if (attempt >= TileX.length) break
    log(`game ${g.n}: not in the test world yet, cancelling and trying tile ${attempt + 1}`)
    await click(g.ws, CANCEL, { rx: 0.5, ry: 0.574 }) // "Connecting to multiplayer game"
    await sleep(5000)
    await shot(g, `after-cancel-${attempt}`)
    if (await atTitle(g)) {
      await click(g.ws, PLAY, { rx: 0.498, ry: 0.474 })
      await sleep(4000)
    }
    await g.ws.call('mouse', { action: 'click', rx: TileX[attempt], ry: TileY }) // OreUI tile, as above
  }
  throw new Error(`game ${g.n} did not enter the world`)
}

async function loadScene () {
  // A query string makes import() read the file again (ES modules are cached by URL).
  return import(`${pathToFileURL(join(here, 'scene.mjs')).href}?t=${Date.now()}`)
}

// Commands go as command_request packets through `send` (lib/bedd.mjs falls back to typing them
// into the chat only when the game refuses `send`); the facing is the bedboo `look` op.
async function setScene (g, scene) {
  await g.ws.command('/kill @e[type=!player]')
  await g.ws.command('/kill @e[type=item]') // what the animals dropped
  await g.ws.command(`/tp @s ${scene.spawn}`)
  const f = scene.facing[g.n]
  if (f) await g.ws.call('look', { yaw: f.yaw, pitch: f.pitch, timeoutMs: 5000 })
  if (g.n === 3) {
    await g.ws.command('/clear @s')
    for (const item of scene.items) await g.ws.command(`/give @s ${item}`)
  }
  await g.ws.call('release')
}

function managerCapture (file) {
  // The manager window straight from the compositor, by title and exe (gfxcapture regexes), scaled
  // into its share of the picture. Its size is left alone (record.ps1 resized it through Win32,
  // which Node cannot call): give it a 1248x1080-ish client area for the sharpest picture.
  const src = `gfxcapture=window_title=${values['manager-title']}:window_exe=(?i)^bedd.exe$:capture_cursor=0:max_framerate=30,` +
    `hwdownload,format=bgra,fps=30,scale=${ManagerW}:${RegionH}:force_original_aspect_ratio=decrease:flags=lanczos,` +
    `pad=${ManagerW}:${RegionH}:0:0:color=0x1b1b1f`
  return ['-hide_banner', '-loglevel', 'error', '-y', '-nostats', '-progress', 'pipe:1', '-stats_period', '0.1',
    '-filter_complex', `${src}[m]`, '-map', '[m]',
    '-t', String(takeSeconds), '-c:v', 'libx264', '-preset', 'ultrafast', '-crf', '10', '-pix_fmt', 'yuv444p', file]
}

// The wall-clock time of the recording's frame 0, from the first -progress block that counts frames.
function firstFrame (child, ms) {
  return new Promise((resolve, reject) => {
    let buf = ''
    const done = (err, t) => {
      clearTimeout(timer)
      child.stdout.removeListener('data', onData)
      child.removeListener('exit', onExit)
      child.stdout.resume() // keep draining the later progress blocks
      if (err) reject(err)
      else resolve(t)
    }
    const onData = chunk => {
      buf += chunk
      const frames = [...buf.matchAll(/^frame=(\d+)\r?$/gm)].map(m => Number(m[1]))
      const n = Math.max(0, ...frames)
      if (n > 0) done(null, Date.now() - n * 1000 / 30)
      else buf = buf.slice(buf.lastIndexOf('\n') + 1) // keep an incomplete line
    }
    const onExit = code => done(new Error(`recording the manager window failed (ffmpeg exit ${code}): is the manager window open?`))
    const timer = setTimeout(() => done(new Error('the manager window recording did not start (no frames)')), ms)
    child.stdout.setEncoding('utf8')
    child.stdout.on('data', onData)
    child.once('exit', onExit)
  })
}

function waitExit (child, ms) {
  return new Promise(resolve => {
    if (child.exitCode !== null) return resolve(true)
    const timer = setTimeout(() => resolve(false), ms)
    child.once('exit', () => { clearTimeout(timer); resolve(true) })
  })
}

async function showTake (take) {
  const scene = await loadScene()
  log(`take ${take}: staging the scene`)
  for (const g of games) await setScene(g, scene)
  await sleep(12000) // command feedback in the chat fades

  const managerFile = join(Out, `take-${take}-manager.mkv`)
  const terminalFile = join(Out, `take-${take}-terminal.mkv`)
  const transcript = join(Out, `take-${take}-terminal.jsonl`)
  const assFile = `take-${take}-terminal.ass`
  const ffmpeg = dry ? 'ffmpeg' : findFFmpeg()
  const capture = managerCapture(managerFile)
  plan(`> ffmpeg ${capture.map(q).join(' ')}`)
  let t0 = Date.now()
  const ffChild = dry ? null : spawn(ffmpeg, capture, { stdio: ['ignore', 'pipe', 'inherit'], windowsHide: true })
  let demo = null
  try {
    if (ffChild) {
      // The terminal pane is timed from the manager recording's first frame (ffmpeg's -progress
      // reports frames as they are written), not from when ffmpeg was started: setting up the
      // capture takes a while.
      t0 = await firstFrame(ffChild, 15000)
    }
    await sleep(2000)
    log(`take ${take}: recording ${takeSeconds} s -> ${managerFile}`)
    const demoArgs = [join(here, 'demo.mjs'), '--pids', games.map(g => g.pid).join(','), '--transcript', transcript]
    plan(`> node ${demoArgs.map(q).join(' ')}`)
    if (dry) demoArgs.splice(3, 2, '--dry-run') // its requests, without writing the transcript
    demo = spawn(process.execPath, demoArgs, { stdio: 'inherit' })
    if (!await waitExit(demo, (takeSeconds + 30) * 1000)) throw new Error('demo.mjs did not finish')
    if (demo.exitCode !== 0) log(`demo.mjs exited with ${demo.exitCode}`)
    await sleep(Math.max(0, t0 + (takeSeconds + 1) * 1000 - Date.now()))
    if (ffChild && !await waitExit(ffChild, 30000)) {
      ffChild.kill()
      throw new Error('ffmpeg did not finish the take')
    }
    if (ffChild && ffChild.exitCode !== 0) throw new Error(`recording the manager window failed (ffmpeg exit ${ffChild.exitCode})`)
  } finally {
    if (demo && demo.exitCode === null) demo.kill()
    if (ffChild && ffChild.exitCode === null) ffChild.kill()
    for (const g of games) { try { await g.ws.call('release') } catch {} }
  }

  // The terminal pane: the transcript drawn on the console's black (libass), as long as the take.
  const termArgs = ['-f', 'lavfi', '-i', `color=c=0x0c0c0c:s=${RegionW - ManagerW}x${RegionH}:r=30:d=${takeSeconds}`,
    '-vf', `ass=${assFile}`, '-c:v', 'libx264', '-preset', 'ultrafast', '-crf', '10', '-pix_fmt', 'yuv444p', terminalFile]
  plan(`fs: ${join(Out, assFile)} from ${transcript}`)
  plan(`> ffmpeg ${termArgs.map(q).join(' ')} (in ${Out})`)
  if (dry) return
  const entries = readFileSync(transcript, 'utf8').split('\n').filter(Boolean).map(l => JSON.parse(l))
  writeFileSync(join(Out, assFile), renderAss(entries, { t0, durationMs: takeSeconds * 1000, width: RegionW - ManagerW, height: RegionH }))
  ff(ffmpeg, termArgs, { cwd: Out })
  log(`take ${take}: ${managerFile}, ${terminalFile}`)
}

// --- run ---------------------------------------------------------------------------------------

let manager = null
let cleaned = false

// Synchronous, so Ctrl+C can run it too.
function cleanup () {
  if (cleaned) return
  cleaned = true
  for (const g of games) g.ws?.close()
  for (const g of games) {
    if (!g.launched) continue
    try { app(['stop', '--slot', String(g.slot), '--json']) } catch (e) { log(`stopping game ${g.n} failed: ${e.message}`) }
  }
  for (const g of games) {
    try { restoreOptions(g) } catch (e) { log(`restoring the options of game ${g.n} failed: ${e.message}`) }
  }
  if (manager && manager.exitCode === null) manager.kill()
  for (const f of ['take', 'stop']) rmSync(join(Out, f), { force: true })
  log('games stopped, options restored')
}

async function main () {
  if (dry) log('dry run: nothing below is executed')
  else mkdirSync(Out, { recursive: true })
  for (const f of ['take', 'stop']) rmSync(join(Out, f), { force: true })

  // The slot locks and the machine-wide locks (signaling, e2e) are the app's: `launch` takes them.
  const others = dry ? [] : listEndpoints()
  if (others.length) log(`note: ${others.length} other BeDD game(s) running; the manager window shows them too`)

  for (const g of games) {
    const info = app(['instance', 'clone', '--slot', String(g.slot), ...(values.base ? ['--base', values.base] : []), '--json'])
    g.name = info.name
    g.dir = info.dir
    // Its world and options are about to be replaced: not under a running game.
    const running = others.find(ep => String(ep.instance).toLowerCase() === String(g.name).toLowerCase())
    if (running) throw new Error(`a game of ${g.name} is already running (pid ${running.pid}): BeDD.exe stop --slot ${g.slot}`)
    // Every mode on (the defaults, spelled out): bedboo ops, `send` for commands, the `ui` tree.
    app(['mod', 'install', g.name, '--set', 'gameHooks=true', '--set', 'allowSend=true', '--set', 'ui=true', '--json'])
    restoreWorld(g)
    setOptions(g)
    log(`prepared ${g.name}`)
  }

  // The manager window: it attaches to every game (a game nobody attaches to closes itself).
  if (!values['no-manager']) {
    plan('> BeDD.exe   (the manager window)')
    if (!dry) manager = spawn(BeDD, [], { detached: true, stdio: 'ignore' })
    await sleep(3000)
  }

  for (const g of games) {
    const ep = app(['launch', '--slot', String(g.slot), '--label', `game ${g.n}`, '--json'])
    g.launched = true
    g.pid = ep.pid
    g.endpoint = ep
    log(`started ${g.name} (pid ${g.pid})`)
    await sleep(3000)
  }
  for (const g of games) {
    g.ws = dry
      ? dryClient(`game ${g.n}`)
      // launch --json prints the endpoint; a game still starting is found by pid once it listens.
      : await connect(g.endpoint.url ? { url: g.endpoint.url, token: g.endpoint.token } : { pid: g.pid }, { timeoutMs: 240000 })
    const deadline = Date.now() + 180000
    while (!await atTitle(g)) {
      if (Date.now() > deadline) { await shot(g, 'no-title'); throw new Error(`game ${g.n}: title screen not found`) }
      await sleep(2000)
    }
    await sleep(3000)
    await enterWorld(g)
    log(`game ${g.n}: entering the world`)
  }
  for (const g of games) await waitInWorld(g)
  await sleep(5000)
  for (const g of games) {
    await g.ws.command('/gamerule sendcommandfeedback false')
    await g.ws.call('window', { clientWidth: ClientW, clientHeight: ClientH })
    await shot(g, 'in-world')
  }

  let take = 1
  const deadline = Date.now() + maxMinutes * 60000
  if (!values['keep-open']) {
    await showTake(take)
    return
  }
  log('ready: create out/take for a take, out/stop to end')
  take = 0
  while (!dry && Date.now() < deadline && !existsSync(join(Out, 'stop'))) {
    if (existsSync(join(Out, 'take'))) {
      rmSync(join(Out, 'take'), { force: true })
      await showTake(++take)
    }
    await sleep(500)
  }
  if (dry) await showTake(1)
}

process.once('SIGINT', () => { cleanup(); process.exit(130) })
try {
  await main()
} catch (e) {
  console.error(`error: ${e.message}`)
  process.exitCode = 1
} finally {
  cleanup()
}
