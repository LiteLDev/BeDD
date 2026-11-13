import { describe, it, before, after, beforeEach, afterEach } from 'node:test'
import assert from 'node:assert/strict'
import { mkdtempSync, mkdirSync, writeFileSync, existsSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { spawnSync } from 'node:child_process'
import { connect, listEndpoints, runDir, commandRequest, BeddError } from '../lib/bedd.mjs'
import { startFakeServer, encodeFrame, decodeFrames } from './helpers/fake-server.mjs'

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms))

function deadPid () {
  return spawnSync(process.execPath, ['-e', '0']).pid
}

describe('frame codec (test helper)', () => {
  it('round-trips 7-bit, 16-bit and 64-bit lengths', () => {
    for (const n of [0, 125, 126, 65535, 65536, 70000]) {
      const payload = Buffer.alloc(n, 7)
      const { frames, rest } = decodeFrames(encodeFrame(2, payload))
      assert.equal(frames.length, 1)
      assert.equal(frames[0].opcode, 2)
      assert.equal(frames[0].payload.length, n)
      assert.equal(rest.length, 0)
    }
  })
})

describe('connect + call', () => {
  let server
  before(async () => {
    server = await startFakeServer({
      handler: async msg => {
        switch (msg.op) {
          case 'ping': return { ok: true, protocol: 2, defaultMode: 'bedboo' }
          case 'slow': await sleep(150); return { ok: true, which: 'slow' }
          case 'fast': return { ok: true, which: 'fast' }
          case 'echo': return { ok: true, args: msg }
          case 'off': return { ok: false, error: 'tap is disabled', code: 'disabled', key: 'tap' }
          case 'nocode': return { ok: false, error: 'boom' }
          case 'silent': return undefined
          case 'stray': return { id: 999999, ok: true }
          case 'big': return { ok: true, data: 'x'.repeat(200000) }
          case 'tap': return { ok: true }
          default: return { ok: false, error: 'unknown op', code: 'unknown_op' }
        }
      }
    })
  })
  after(() => server.close())

  it('sends {id, op, ...args} and resolves with the matching reply', async () => {
    const g = await connect({ url: server.url, token: server.token })
    try {
      const r = await g.call('ping')
      assert.equal(r.ok, true)
      assert.equal(r.protocol, 2)
      const e = await g.call('echo', { yaw: 90, pitch: 5 })
      assert.equal(e.args.op, 'echo')
      assert.equal(e.args.yaw, 90)
      assert.equal(typeof e.args.id, 'number')
    } finally {
      g.close()
    }
  })

  it('matches replies that arrive out of order', async () => {
    const g = await connect({ url: server.url, token: server.token })
    try {
      const slow = g.call('slow')
      const fast = g.call('fast')
      const order = []
      slow.then(() => order.push('slow'))
      fast.then(() => order.push('fast'))
      assert.equal((await slow).which, 'slow')
      assert.equal((await fast).which, 'fast')
      assert.deepEqual(order, ['fast', 'slow'])
    } finally {
      g.close()
    }
  })

  it('turns {ok:false} into a BeddError with .code and .key', async () => {
    const g = await connect({ url: server.url, token: server.token })
    try {
      await assert.rejects(g.call('off'), err => {
        assert.ok(err instanceof BeddError)
        assert.equal(err.code, 'disabled')
        assert.equal(err.key, 'tap')
        assert.match(err.message, /tap is disabled/)
        assert.equal(err.reply.ok, false)
        return true
      })
      await assert.rejects(g.call('nocode'), { code: 'failed' })
      await assert.rejects(g.call('nope'), { code: 'unknown_op' })
    } finally {
      g.close()
    }
  })

  it('times out a call without a reply and ignores replies to unknown ids', async () => {
    const g = await connect({ url: server.url, token: server.token })
    try {
      await assert.rejects(g.call('silent', {}, { timeoutMs: 100 }), { code: 'timeout' })
      await assert.rejects(g.call('stray', {}, { timeoutMs: 100 }), { code: 'timeout' })
      assert.equal((await g.call('ping')).ok, true) // still usable
    } finally {
      g.close()
    }
  })

  it('receives replies larger than 64 KiB', async () => {
    const g = await connect({ url: server.url, token: server.token })
    try {
      assert.equal((await g.call('big')).data.length, 200000)
    } finally {
      g.close()
    }
  })

  it('hands binary messages to onBinary', async () => {
    const got = []
    const g = await connect({ url: server.url, token: server.token }, { onBinary: b => got.push(b) })
    try {
      await g.call('tap', { action: 'subscribe' })
      for (const c of server.connections) c.sendBinary(Buffer.from([1, 2, 3]))
      for (let i = 0; i < 50 && got.length === 0; i++) await sleep(10)
      assert.deepEqual([...got[0]], [1, 2, 3])
    } finally {
      g.close()
    }
  })

  it('rejects pending and later calls once closed', async () => {
    const g = await connect({ url: server.url, token: server.token })
    const pending = g.call('silent', {}, { timeoutMs: 5000 })
    g.close()
    await assert.rejects(pending, { code: 'closed' })
    await assert.rejects(g.call('ping'), { code: 'closed' })
    assert.equal(g.closed, true)
  })

  it('rejects pending calls when the server drops the connection', async () => {
    const g = await connect({ url: server.url, token: server.token })
    const pending = g.call('silent', {}, { timeoutMs: 5000 })
    await sleep(50)
    for (const c of server.connections) c.socket.destroy()
    await assert.rejects(pending, { code: 'closed' })
  })

  it('fails to connect with a wrong token (HTTP 401), after retrying until the deadline', async () => {
    const t = Date.now()
    await assert.rejects(connect({ url: server.url, token: 'b'.repeat(64) }, { timeoutMs: 600 }), { code: 'connect_failed' })
    assert.ok(Date.now() - t >= 550)
  })

  it('retries a server that is not listening yet', async () => {
    const late = await startFakeServer({ handler: () => ({ ok: true, late: true }) })
    const { url, token } = late
    await late.close() // the port is free now: connections are refused
    const port = Number(new URL(url).port)
    let again
    setTimeout(async () => { again = await startFakeServer({ token, port, handler: () => ({ ok: true, late: true }) }) }, 400)
    const g = await connect({ url, token }, { timeoutMs: 5000 })
    try {
      assert.equal((await g.call('ping')).late, true)
    } finally {
      g.close()
      await again.close()
    }
  })
})

describe('discovery', () => {
  let server, home, saved
  before(async () => { server = await startFakeServer({ handler: () => ({ ok: true, pong: true }) }) })
  after(() => server.close())
  beforeEach(() => {
    saved = process.env.BEDD_HOME
    home = mkdtempSync(join(tmpdir(), 'bedd-test-'))
    process.env.BEDD_HOME = home
    mkdirSync(join(home, 'run'))
  })
  afterEach(() => {
    if (saved === undefined) delete process.env.BEDD_HOME
    else process.env.BEDD_HOME = saved
    rmSync(home, { recursive: true, force: true })
  })

  const write = (pid, extra = {}) => {
    const file = join(home, 'run', `${pid}.json`)
    writeFileSync(file, JSON.stringify({
      protocol: 2, kind: 'client', pid, url: server.url, token: server.token,
      instance: '1.26.60.05-51', instanceDir: 'C:\\x', label: 'game 1', modVersion: '2.0.0',
      startedAt: new Date().toISOString(), ...extra
    }))
    return file
  }

  it('reads %BEDD_HOME%\\run', () => {
    assert.equal(runDir(), join(home, 'run'))
  })

  it('connects by pid and by instance (ignoring case)', async () => {
    write(process.pid)
    const a = await connect({ pid: process.pid })
    assert.equal(a.pid, process.pid)
    assert.equal((await a.call('ping')).pong, true)
    a.close()
    const b = await connect({ instance: '1.26.60.05-51'.toUpperCase() })
    assert.equal(b.endpoint.label, 'game 1')
    b.close()
  })

  it('ignores files of dead processes and prunes them on request', async () => {
    const dead = write(deadPid())
    write(process.pid)
    writeFileSync(join(home, 'run', 'garbage.json'), '{not json')
    assert.equal(listEndpoints().length, 1)
    assert.ok(existsSync(dead))
    listEndpoints({ prune: true })
    assert.ok(!existsSync(dead))
  })

  it('waits for a game that is still starting, then gives up', async () => {
    setTimeout(() => write(process.pid), 300)
    const g = await connect({ pid: process.pid }, { timeoutMs: 3000 })
    g.close()
    await assert.rejects(connect({ instance: 'missing' }, { timeoutMs: 300 }), { code: 'not_found' })
  })
})

describe('commandRequest', () => {
  it('matches the command_request layout of a 1.26.51 client capture', () => {
    // Captured: "/time set 1000", origin player with uuid c62ea01d..., request_id "", entity 0,
    // internal false, version "latest".
    const uuid = Buffer.from('c62ea01dcd943510856f642caf327cfb', 'hex')
    const expected = '4d0e2f74696d6520736574203130303006706c61796572c62ea01dcd943510856f642caf327cfb' +
      '00000000000000000000066c6174657374'
    assert.equal(Buffer.from(commandRequest('/time set 1000', uuid), 'base64').toString('hex'), expected)
    assert.equal(Buffer.from(commandRequest('time set 1000', uuid), 'base64').toString('hex'), expected)
  })

  it('command() sends it through `send`, or types it when `send` is refused', async () => {
    let refuse = false
    const seen = []
    const server = await startFakeServer({
      handler: msg => {
        seen.push(msg.op)
        if (msg.op === 'send' && refuse) return { ok: false, error: 'not allowed', code: 'refused' }
        return { ok: true }
      }
    })
    try {
      const g = await connect({ url: server.url, token: server.token })
      await g.command('/say hi')
      assert.deepEqual(seen, ['send'])
      refuse = true
      seen.length = 0
      await g.command('/say hi')
      assert.deepEqual(seen, ['send', 'key', 'text', 'key'])
      assert.equal(server.requests.at(-2).text, '/say hi')
      g.close()
    } finally {
      await server.close()
    }
  })
})
