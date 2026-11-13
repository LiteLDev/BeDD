#!/usr/bin/env node
// The script the showcase video shows in its terminal pane (record.mjs runs it during a take).
// Each game gets its own WebSocket and runs its steps concurrently with the others; every line
// printed is exactly the code that runs (the step's own source).
//
// It also shows the order BeDD asks scripts to reach for things in: bedboo ops first (`look`,
// `input`, `action`, `send` move the player and talk to the game without a keyboard), the `ui` tree
// to find a control by name, and the plain keyboard/mouse ops only where nothing semantic exists.

import { parseArgs } from 'node:util'
import { pathToFileURL } from 'node:url'
import { appendFileSync, writeFileSync } from 'node:fs'
import { connect, commandRequest } from './lib/bedd.mjs'
import { find } from './lib/ui.mjs'
import { ansi } from './lib/terminal.mjs'
import { dryClient } from './lib/dry.mjs'

const HELP = `usage: node demo.mjs (--pids <pid,...> | --instances <name,...>) [--transcript <file>] [--dry-run]

Drives up to three running BeDD games over WebSocket (the discovery files in
%LOCALAPPDATA%\\BeDD\\run, BEDD_HOME moves them) and prints every step it runs.

  --pids <list>         game process ids, game 1 first
  --instances <list>    instance folder names instead of pids
  --transcript <file>   also write each printed line with its time (JSON lines) for the video
  --dry-run             print the WebSocket requests instead of connecting
  -h, --help            this text`

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms))

// [delay before the step (ms), the step]. The steps get the game's client as g.
export const scripts = {
  // Game 1 looks along the river and jumps: `look` turns the player through the game's own camera
  // code and replies once turned, `input` jumps through its input pipeline. No mouse counts, no keys.
  1: [
    [300, async g => { for (let yaw = 0; yaw <= 36; yaw += 2) await g.call('look', { yaw, pitch: 2 }) }],
    [300, g => g.call('input', { jump: true, ttlMs: 300 })],
    [700, async g => { for (let yaw = 36; yaw >= 9; yaw -= 1.5) await g.call('look', { yaw, pitch: 2 }) }]
  ],
  // Game 2 runs commands: a command_request packet through `send`, as the chat screen would send
  // it, without opening the chat and typing.
  2: [
    [1600, g => g.call('send', { buf: commandRequest('/summon sheep ^ ^ ^3') })],
    [1900, g => g.call('send', { buf: commandRequest('/summon cow ^2 ^ ^4') })]
  ],
  // Game 3 sorts its inventory. `action` opens it; `find` looks the sword up in the UI tree (the
  // point after it is where the slot sits, used only when the screen has no tree to read). An empty
  // cell has no name to find it by, the last move only parks the pointer for the picture, and no
  // bedboo op closes a screen (the game closes containers on ESC): those three are mouse/key ops.
  3: [
    [1800, g => g.call('action', { name: 'inventory' })],
    [900, async g => g.call('mouse', { action: 'move', ...await find(g, /diamond sword/i, { rx: 0.504, ry: 0.644 }) })],
    [1400, g => g.call('mouse', { action: 'click' })],
    [600, g => g.call('mouse', { action: 'click', rx: 0.588, ry: 0.562 })],
    [900, g => g.call('mouse', { action: 'move', rx: 0.61, ry: 0.644 })],
    [1600, g => g.call('key', { key: 'ESC' })]
  ]
}

const COLOR = { 1: 'cyan', 2: 'yellow', 3: 'green' }

// The source of a step as the viewer reads it: "g => " and a block's braces dropped.
export function sourceOf (fn) {
  let s = fn.toString().replace(/^async\s+/, '').replace(/^g\s*=>\s*/, '').trim()
  if (s.startsWith('{') && s.endsWith('}')) s = s.slice(1, -1).trim()
  return s.replace(/\s+/g, ' ')
}

function makePrinter (transcript) {
  if (transcript) writeFileSync(transcript, '')
  return parts => {
    console.log(ansi(parts))
    if (transcript) appendFileSync(transcript, JSON.stringify({ t: Date.now(), parts }) + '\n')
  }
}

async function main () {
  const { values } = parseArgs({
    options: {
      pids: { type: 'string' },
      instances: { type: 'string' },
      transcript: { type: 'string' },
      'dry-run': { type: 'boolean' },
      help: { type: 'boolean', short: 'h' }
    }
  })
  if (values.help) {
    console.log(HELP)
    return
  }
  const targets = values.pids
    ? values.pids.split(',').map(p => ({ pid: Number(p) }))
    : values.instances?.split(',').map(instance => ({ instance }))
  if (!targets?.length && !values['dry-run']) {
    console.error(HELP)
    process.exitCode = 2
    return
  }
  const dry = values['dry-run']
  const games = targets ?? [{}, {}, {}]
  const print = makePrinter(values.transcript)

  const clients = await Promise.all(games.slice(0, 3).map((t, i) =>
    dry ? dryClient(`game ${i + 1}`) : connect(t, { timeoutMs: 10000 })))
  try {
    print([{ text: 'C:\\src\\my-client-mod> ', color: 'gray' }, { text: 'node e2e.mjs --games 3' }])
    print([{ text: `[1] [2] [3]: ${clients.length} headless games, one WebSocket each`, color: 'gray' }])
    print([{ text: '' }])
    await Promise.all(clients.map(async (g, i) => {
      const n = i + 1
      for (const [delay, step] of scripts[n]) {
        await sleep(dry ? 0 : delay)
        print([{ text: `[${n}] `, color: COLOR[n] }, { text: sourceOf(step) }])
        await step(g)
      }
    }))
    print([{ text: '' }])
    print([{ text: `Done: ${clients.length} games driven, your mouse and keyboard untouched.`, color: 'green' }])
  } finally {
    for (const g of clients) {
      // Nothing stays held when the script ends (the game also releases a closed connection's input).
      try { await g.call('release') } catch {}
      g.close()
    }
  }
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  main().catch(e => {
    console.error(`error: ${e.message}`)
    process.exitCode = 1
  })
}
