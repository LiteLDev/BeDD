// A small BeDD client for Node 24: no dependencies, the global WebSocket (undici) and the discovery
// files a game writes while its WebSocket server listens (%LOCALAPPDATA%\BeDD\run\<pid>.json, the
// folder moved by BEDD_HOME). See docs/reference/protocol.md for the ops.
//
//   import { connect } from './lib/bedd.mjs'
//   const game = await connect({ pid: 1234 })            // or { instance: '1.26.60.05-2' } or { url, token }
//   await game.call('look', { yaw: 90, pitch: 10 })     // -> the reply ({ id, ok: true, ... })
//   await game.command('/time set day')                  // a command_request through `send`
//   game.close()
//
// A reply {ok:false, error, code, key} rejects with a BeddError carrying .code, .key and .reply.

import { readdirSync, readFileSync, rmSync } from 'node:fs'
import { join } from 'node:path'
import { randomBytes } from 'node:crypto'

export const PROTOCOL = 2

export class BeddError extends Error {
  constructor (message, { code, key, reply } = {}) {
    super(message)
    this.name = 'BeddError'
    this.code = code
    if (key !== undefined) this.key = key
    if (reply !== undefined) this.reply = reply
  }
}

// --- discovery ---------------------------------------------------------------------------------

// BeDD's folder: BEDD_HOME, else %LOCALAPPDATA%\BeDD (read on every call, so tests can move it).
export function beddHome () {
  const home = process.env.BEDD_HOME?.trim()
  if (home) return home
  const local = process.env.LOCALAPPDATA ?? join(process.env.USERPROFILE ?? '.', 'AppData', 'Local')
  return join(local, 'BeDD')
}

export function runDir () {
  return join(beddHome(), 'run')
}

export function isAlive (pid) {
  if (!Number.isInteger(pid) || pid <= 0) return false
  try {
    process.kill(pid, 0)
    return true
  } catch (e) {
    return e.code === 'EPERM' // exists, but not ours to signal
  }
}

// The endpoints of running games and servers, oldest first. Files that cannot be read (being
// written or deleted right now) are skipped; with prune, files of processes that are gone are
// deleted, as the app does.
export function listEndpoints ({ prune = false, dir = runDir() } = {}) {
  let names
  try {
    names = readdirSync(dir)
  } catch {
    return []
  }
  const result = []
  for (const name of names) {
    if (!name.toLowerCase().endsWith('.json')) continue
    const file = join(dir, name)
    let ep
    try {
      ep = JSON.parse(readFileSync(file, 'utf8').replace(/^﻿/, ''))
    } catch {
      continue
    }
    if (!ep || typeof ep.url !== 'string' || !ep.url || !Number.isInteger(ep.pid) || ep.pid <= 0) continue
    if (!isAlive(ep.pid)) {
      if (prune) {
        try { rmSync(file, { force: true }) } catch {}
      }
      continue
    }
    result.push({ ...ep, file })
  }
  const time = ep => {
    const t = Date.parse(ep.startedAt ?? '')
    return Number.isNaN(t) ? 0 : t
  }
  result.sort((a, b) => time(a) - time(b))
  return result
}

// The endpoint a target names: { url, token } as is, { pid } or { instance } (folder name, ignoring
// case; the newest game of that instance) from the discovery files. Null when none matches yet.
export function findEndpoint (target, { dir } = {}) {
  if (target.url) return { ...target }
  const all = listEndpoints({ dir })
  if (target.pid !== undefined) return all.find(ep => ep.pid === Number(target.pid)) ?? null
  if (target.instance !== undefined) {
    const want = String(target.instance).toLowerCase()
    const hits = all.filter(ep => String(ep.instance ?? '').toLowerCase() === want)
    return hits.at(-1) ?? null
  }
  throw new BeddError('connect needs { url, token }, { pid } or { instance }', { code: 'bad_args' })
}

// --- connection --------------------------------------------------------------------------------

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms))

function withToken (url, token) {
  if (!token) return url
  return url + (url.includes('?') ? '&' : '?') + 'token=' + encodeURIComponent(token)
}

// Opens a WebSocket to a game. Within timeoutMs it waits for the discovery file of a game that is
// still starting (pid / instance) and retries a connection that fails (a game not listening yet, a
// discovery file left by a game that just went away).
export async function connect (target, { timeoutMs = 10000, callTimeoutMs = 30000, onBinary, dir } = {}) {
  const deadline = Date.now() + timeoutMs
  for (;;) {
    const ep = findEndpoint(target, { dir })
    if (ep) {
      try {
        const ws = await open(withToken(ep.url, ep.token), Math.max(1000, deadline - Date.now()))
        return new Bedd(ws, ep, { onBinary, callTimeoutMs })
      } catch (e) {
        if (Date.now() >= deadline) throw e
      }
    } else if (Date.now() >= deadline) {
      throw new BeddError(`no running BeDD game for ${JSON.stringify(target)} in ${dir ?? runDir()}`, { code: 'not_found' })
    }
    await sleep(250)
  }
}

function open (url, timeoutMs) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url)
    ws.binaryType = 'arraybuffer'
    let done = false
    const finish = err => {
      if (done) return
      done = true
      clearTimeout(timer)
      ws.removeEventListener('open', onOpen)
      ws.removeEventListener('error', onError)
      ws.removeEventListener('close', onError)
      if (err) {
        try { ws.close() } catch {}
        reject(err)
      } else {
        resolve(ws)
      }
    }
    const onOpen = () => finish()
    // The server answers a bad token with HTTP 401 and a foreign Origin with 403: both end here.
    const onError = () => finish(new BeddError(`could not connect to ${url.replace(/token=[^&]*/, 'token=***')}`, { code: 'connect_failed' }))
    const timer = setTimeout(() => finish(new BeddError('timed out connecting', { code: 'timeout' })), Math.max(1, timeoutMs))
    ws.addEventListener('open', onOpen)
    ws.addEventListener('error', onError)
    ws.addEventListener('close', onError)
  })
}

export class Bedd {
  #ws
  #nextId = 1
  #pending = new Map() // id -> { resolve, reject, timer, op }
  #callTimeoutMs
  #onBinary
  #closed = false

  constructor (ws, endpoint, { onBinary, callTimeoutMs = 30000 } = {}) {
    this.#ws = ws
    this.endpoint = endpoint
    this.#onBinary = onBinary
    this.#callTimeoutMs = callTimeoutMs
    ws.addEventListener('message', e => this.#onMessage(e.data))
    ws.addEventListener('close', () => this.#fail(new BeddError('connection closed', { code: 'closed' })))
  }

  get closed () { return this.#closed }
  get pid () { return this.endpoint.pid }

  // Sends {id, op, ...args} and resolves with the reply that carries the same id. Replies may come
  // in any order (several calls in flight on one connection). {ok:false} rejects with a BeddError.
  call (op, args = {}, { timeoutMs = this.#callTimeoutMs } = {}) {
    if (this.#closed) return Promise.reject(new BeddError('connection closed', { code: 'closed' }))
    const id = this.#nextId++
    return new Promise((resolve, reject) => {
      const timer = timeoutMs > 0
        ? setTimeout(() => {
          this.#pending.delete(id)
          reject(new BeddError(`${op}: no reply within ${timeoutMs} ms`, { code: 'timeout' }))
        }, timeoutMs)
        : null
      this.#pending.set(id, { resolve, reject, timer, op })
      try {
        this.#ws.send(JSON.stringify({ ...args, id, op }))
      } catch (e) {
        clearTimeout(timer)
        this.#pending.delete(id)
        reject(new BeddError(`${op}: ${e.message}`, { code: 'closed' }))
      }
    })
  }

  // Runs a chat command as the player: a command_request packet through `send` (bedboo mode, no
  // keyboard). When the game refuses `send` (allowSend off, id not in sendAllow, bytes it does not
  // read) it falls back to what a player does, as bedboo's router does: T opens the chat, `text`
  // types the line, ENTER sends it.
  async command (line) {
    try {
      return await this.call('send', { buf: commandRequest(line) })
    } catch (e) {
      if (!['disabled', 'refused', 'bad_args', 'unsupported'].includes(e.code)) throw e
    }
    await this.call('key', { key: 'T' })
    await sleep(700) // the chat screen opens on a later frame; typed characters before it are lost
    await this.call('text', { text: line })
    return this.call('key', { key: 'ENTER' })
  }

  close () {
    if (this.#closed) return
    this.#fail(new BeddError('connection closed', { code: 'closed' }))
    try { this.#ws.close(1000) } catch {}
  }

  #fail (err) {
    this.#closed = true
    for (const [id, p] of this.#pending) {
      clearTimeout(p.timer)
      this.#pending.delete(id)
      p.reject(err)
    }
  }

  #onMessage (data) {
    if (typeof data !== 'string') {
      this.#onBinary?.(Buffer.from(data)) // tap frames after {op:"tap", action:"subscribe"}
      return
    }
    let msg
    try {
      msg = JSON.parse(data)
    } catch {
      return
    }
    const p = this.#pending.get(msg?.id)
    if (!p) return
    this.#pending.delete(msg.id)
    clearTimeout(p.timer)
    if (msg.ok === false) {
      p.reject(new BeddError(`${p.op}: ${msg.error ?? 'failed'}`, { code: msg.code ?? 'failed', key: msg.key, reply: msg }))
    } else {
      p.resolve(msg)
    }
  }
}

// --- packets for `send` ------------------------------------------------------------------------

function varint (n) {
  const out = []
  let v = n >>> 0
  while (v >= 0x80) {
    out.push((v & 0x7f) | 0x80)
    v >>>= 7
  }
  out.push(v)
  return Buffer.from(out)
}

function str (s) {
  const b = Buffer.from(s, 'utf8')
  return Buffer.concat([varint(b.length), b])
}

// The base64 of a serverbound command_request (id 77) as the game reads it in 1.26.5x/1.26.60:
// command, origin {type "player", uuid, request_id "", player_entity_id int64 0}, internal false,
// version "latest" (layout taken from a client capture against BDS 1.26.51).
export function commandRequest (line, uuid = randomBytes(16)) {
  const command = line.startsWith('/') ? line : '/' + line
  return Buffer.concat([
    varint(77),
    str(command),
    str('player'),
    Buffer.from(uuid),
    str(''),
    Buffer.alloc(8), // player_entity_id
    Buffer.from([0]), // internal
    str('latest')
  ]).toString('base64')
}
