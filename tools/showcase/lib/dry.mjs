// --dry-run stand-ins: a game client that prints every WebSocket request it would send and answers
// with plausible replies, so record.mjs and demo.mjs walk their whole plan without a game.

import { commandRequest } from './bedd.mjs'

// A 1x1 PNG, the dry screenshot.
const PNG = 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg=='

function reply (op, args) {
  switch (op) {
    case 'ping':
      return { protocol: 2, defaultMode: 'bedboo', modes: { bedboo: [], input: [], ui: ['ui'] } }
    case 'world':
      return { inLevel: true, levelEpoch: 1, topScreen: 'hud_screen', player: { rot: { yaw: 0, pitch: 0 }, slot: 0 } }
    case 'ui':
      // No snapshot: the scripts take their documented coordinate fallbacks, which shows them.
      return args.action ? {} : { snapshot: 'dry', screens: [], active: null, roots: [], nodes: {}, truncated: false }
    case 'screenshot':
      return { width: 1, height: 1, format: 'png', data: PNG }
    case 'look':
      return { inLevel: true, yaw: args.yaw, pitch: args.pitch }
    default:
      return {}
  }
}

export function dryClient (name, print = s => console.log(s)) {
  let id = 0
  const client = {
    pid: 0,
    endpoint: { pid: 0, url: 'ws://127.0.0.1:<port>/v1', token: '<token>', label: name },
    closed: false,
    async call (op, args = {}) {
      const req = { id: ++id, op, ...args }
      if (typeof req.buf === 'string' && req.buf.length > 48) req.buf = req.buf.slice(0, 45) + '...'
      print(`  ws ${name} > ${JSON.stringify(req)}`)
      return { id: req.id, ok: true, ...reply(op, args) }
    },
    async command (line) {
      print(`  ws ${name} > {"id":${++id},"op":"send","buf":"<command_request ${JSON.stringify(line)}>"}`)
      commandRequest(line) // what the real client sends: built here so a bad line fails the dry run too
      return { id, ok: true, packetId: 77, name: 'command_request' }
    },
    close () { client.closed = true }
  }
  return client
}
