// WebSocket protocol tests against a running BeDD game (Node 24, no dependencies):
//   node --test tests/ws/
// The game is found through its discovery file, %LOCALAPPDATA%\BeDD\run\<pid>.json (root: env
// BEDD_HOME): env BEDD_PID picks one, else the newest file of a live process. Without a running
// game every test is skipped.
import { describe, it, before } from 'node:test';
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import fs from 'node:fs';
import net from 'node:net';
import path from 'node:path';

function runDir() {
  const home = process.env.BEDD_HOME || path.join(process.env.LOCALAPPDATA || '', 'BeDD');
  return path.join(home, 'run');
}

function alive(pid) {
  try {
    process.kill(pid, 0);
    return true;
  } catch (e) {
    return e.code === 'EPERM';
  }
}

function findEndpoint() {
  const dir = runDir();
  if (!fs.existsSync(dir)) return null;
  const wanted = process.env.BEDD_PID ? `${process.env.BEDD_PID}.json` : null;
  const files = fs
    .readdirSync(dir)
    .filter((f) => f.endsWith('.json') && (!wanted || f === wanted))
    .map((f) => path.join(dir, f))
    .sort((a, b) => fs.statSync(b).mtimeMs - fs.statSync(a).mtimeMs);
  for (const file of files) {
    try {
      const ep = JSON.parse(fs.readFileSync(file, 'utf8'));
      if (ep.protocol === 2 && alive(ep.pid)) return ep;
    } catch {
      // being written, or not ours
    }
  }
  return null;
}

const ep = findEndpoint();
const skip = ep ? false : 'no running BeDD game (no discovery file of a live process)';
const isClient = ep?.kind === 'client';

// A connection with request/reply by id, and the binary messages it received.
class Client {
  constructor(ws) {
    this.ws = ws;
    this.nextId = 1;
    this.pending = new Map();
    this.binary = [];
    this.binaryWaiters = [];
    ws.binaryType = 'arraybuffer';
    this.closed = new Promise((resolve) => ws.addEventListener('close', (e) => resolve({ code: e.code, reason: e.reason })));
    ws.addEventListener('message', (e) => {
      if (typeof e.data === 'string') {
        const reply = JSON.parse(e.data);
        const p = this.pending.get(reply.id);
        if (p) {
          this.pending.delete(reply.id);
          p(reply);
        }
      } else {
        this.binary.push(Buffer.from(e.data));
        for (const w of this.binaryWaiters.splice(0)) w();
      }
    });
  }

  static open(url = `${ep.url}?token=${ep.token}`) {
    return new Promise((resolve, reject) => {
      const ws = new WebSocket(url);
      const c = new Client(ws);
      ws.addEventListener('open', () => resolve(c), { once: true });
      ws.addEventListener('error', () => reject(new Error('websocket error')), { once: true });
    });
  }

  request(body, timeoutMs = 15000) {
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error(`no reply to ${JSON.stringify(body)}`)), timeoutMs);
      this.pending.set(id, (r) => {
        clearTimeout(timer);
        resolve(r);
      });
      this.ws.send(JSON.stringify({ id, ...body }));
    });
  }

  async nextBinary(timeoutMs = 10000) {
    const deadline = Date.now() + timeoutMs;
    while (this.binary.length === 0) {
      if (Date.now() > deadline) throw new Error('no binary message');
      await new Promise((resolve) => {
        const t = setTimeout(resolve, 200);
        this.binaryWaiters.push(() => {
          clearTimeout(t);
          resolve();
        });
      });
    }
    return this.binary.shift();
  }

  close() {
    this.ws.close();
    return this.closed;
  }
}

// A hand-made upgrade request: the HTTP status line and headers of the answer.
function rawUpgrade({ target = '/v1', headers = {} } = {}) {
  const { port } = new URL(ep.url);
  return new Promise((resolve, reject) => {
    const sock = net.connect(Number(port), '127.0.0.1');
    let buf = '';
    sock.setEncoding('latin1');
    sock.on('error', reject);
    sock.on('data', (d) => {
      buf += d;
      const end = buf.indexOf('\r\n\r\n');
      if (end < 0) return;
      const [status, ...lines] = buf.slice(0, end).split('\r\n');
      const h = {};
      for (const l of lines) {
        const i = l.indexOf(':');
        h[l.slice(0, i).trim().toLowerCase()] = l.slice(i + 1).trim();
      }
      sock.destroy();
      resolve({ status: Number(status.split(' ')[1]), headers: h });
    });
    const all = {
      Host: `127.0.0.1:${port}`,
      Upgrade: 'websocket',
      Connection: 'Upgrade',
      'Sec-WebSocket-Key': Buffer.from('0123456789abcdef').toString('base64'),
      'Sec-WebSocket-Version': '13',
      ...headers,
    };
    sock.write(`GET ${target} HTTP/1.1\r\n${Object.entries(all).map(([k, v]) => `${k}: ${v}`).join('\r\n')}\r\n\r\n`);
  });
}

describe('BeDD WebSocket protocol', { skip }, () => {
  before(() => {
    assert.match(ep.url, /^ws:\/\/127\.0\.0\.1:\d+\/v1$/);
    assert.match(ep.token, /^[0-9a-f]{64}$/);
  });

  it('discovery file has the contract fields', () => {
    for (const k of ['protocol', 'kind', 'pid', 'url', 'token', 'instance', 'instanceDir', 'label', 'modVersion', 'startedAt'])
      assert.ok(k in ep, `missing ${k}`);
    assert.ok(['client', 'server'].includes(ep.kind));
  });

  it('upgrades with the token in the query', async () => {
    const r = await rawUpgrade({ target: `/v1?token=${ep.token}` });
    assert.equal(r.status, 101);
    const key = Buffer.from('0123456789abcdef').toString('base64');
    const accept = crypto.createHash('sha1').update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
    assert.equal(r.headers['sec-websocket-accept'], accept);
  });

  it('upgrades with the token in an Authorization header', async () => {
    const r = await rawUpgrade({ headers: { Authorization: `Bearer ${ep.token}` } });
    assert.equal(r.status, 101);
  });

  it('answers 401 without a token or with a wrong one', async () => {
    assert.equal((await rawUpgrade()).status, 401);
    assert.equal((await rawUpgrade({ target: `/v1?token=${'0'.repeat(64)}` })).status, 401);
    assert.equal((await rawUpgrade({ headers: { Authorization: 'Bearer nope' } })).status, 401);
  });

  it('answers 403 to a request with an Origin header', async () => {
    const r = await rawUpgrade({ target: `/v1?token=${ep.token}`, headers: { Origin: 'http://example.com' } });
    assert.equal(r.status, 403);
  });

  it('answers 404 off /v1', async () => {
    assert.equal((await rawUpgrade({ target: `/v2?token=${ep.token}` })).status, 404);
  });

  it('ping: protocol 2 and the modes', async () => {
    const c = await Client.open();
    try {
      const r = await c.request({ op: 'ping' });
      assert.equal(r.ok, true);
      assert.equal(r.id, 1);
      assert.equal(r.protocol, 2);
      assert.equal(r.pid, ep.pid);
      if (isClient) {
        assert.equal(r.defaultMode, 'bedboo');
        assert.deepEqual(r.modes.bedboo, ['world', 'look', 'input', 'slot', 'action', 'button', 'settings', 'send', 'stack', 'timescale', 'join', 'tap']);
        assert.deepEqual(r.modes.input, ['key', 'text', 'mouse', 'raw', 'release', 'flush', 'focus', 'window', 'screenshot']);
        assert.deepEqual(r.modes.ui, ['ui', 'oreui']);
        assert.ok('hwnd' in r);
      }
    } finally {
      await c.close();
    }
  });

  it('errors keep the envelope', async () => {
    const c = await Client.open();
    try {
      const r = await c.request({ op: 'no-such-op' });
      assert.equal(r.ok, false);
      assert.equal(r.id, 1);
      assert.match(r.error, /unknown op/);
      c.ws.send('not json');
      // Not JSON: an error reply without an id; the connection stays usable.
      const next = await c.request({ op: 'ping' });
      assert.equal(next.ok, true);
    } finally {
      await c.close();
    }
  });

  it('serves several connections at once, each in order', async () => {
    const clients = await Promise.all([1, 2, 3, 4].map(() => Client.open()));
    try {
      const replies = await Promise.all(clients.flatMap((c) => [c.request({ op: 'ping' }), c.request({ op: 'state' })]));
      for (const r of replies) assert.equal(r.ok, true);
      const state = await clients[0].request({ op: 'state' });
      assert.ok(state.clients >= 4, `clients ${state.clients}`);
    } finally {
      await Promise.all(clients.map((c) => c.close()));
    }
  });

  it('refuses an oversize message (close 1009)', async () => {
    const c = await Client.open();
    c.ws.send('x'.repeat(16 * 1024 * 1024 + 1));
    const { code } = await c.closed;
    assert.equal(code, 1009);
  });

  it('tap: subscribe gives binary frames starting with HELLO', { skip: !isClient }, async () => {
    const c = await Client.open();
    try {
      const status = await c.request({ op: 'tap' });
      if (!status.ok) {
        assert.equal(status.code, 'disabled');
        assert.equal(status.key, 'tap');
        return;
      }
      const r = await c.request({ op: 'tap', action: 'subscribe' });
      assert.equal(r.ok, true, JSON.stringify(r));
      const second = await Client.open();
      try {
        const busy = await second.request({ op: 'tap', action: 'subscribe' });
        assert.equal(busy.code, 'busy');
      } finally {
        await second.close();
      }
      const frame = await c.nextBinary();
      const len = frame.readUInt32LE(0);
      assert.equal(len, frame.length - 4);
      assert.equal(frame[4], 4); // MARKER
      const hello = JSON.parse(frame.subarray(24).toString('utf8'));
      assert.equal(hello.m, 'HELLO');
      assert.equal(hello.pid, ep.pid);
      const begin = JSON.parse((await c.nextBinary()).subarray(24).toString('utf8'));
      assert.equal(begin.m, 'REPLAY_BEGIN');
      // Requests still work on the subscribed connection.
      assert.equal((await c.request({ op: 'ping' })).ok, true);
    } finally {
      await c.close();
    }
    // The tap is free again.
    const again = await Client.open();
    try {
      let r;
      for (let i = 0; i < 20; i++) {
        r = await again.request({ op: 'tap' });
        if (!r.subscribed) break;
        await new Promise((res) => setTimeout(res, 100));
      }
      assert.equal(r.subscribed, false);
    } finally {
      await again.close();
    }
  });

  it('a closed connection releases the keys it held', { skip: !isClient }, async () => {
    const vk = 0x4a; // J: does nothing on the title screen or in a world
    const a = await Client.open();
    const down = await a.request({ op: 'key', key: 'J', action: 'down' });
    assert.equal(down.ok, true, JSON.stringify(down));
    const watcher = await Client.open();
    try {
      let s = await watcher.request({ op: 'state' });
      const held = s.held.find((h) => h.vk === vk);
      assert.ok(held, `J not held: ${JSON.stringify(s.held)}`);
      await a.close();
      for (let i = 0; i < 30; i++) {
        s = await watcher.request({ op: 'state' });
        if (!s.held.some((h) => h.vk === vk)) break;
        await new Promise((res) => setTimeout(res, 100));
      }
      assert.ok(!s.held.some((h) => h.vk === vk), `J still held: ${JSON.stringify(s.held)}`);
    } finally {
      await watcher.close();
    }
  });
  it('ui: a text view whose refs work from another connection', { skip: !isClient }, async () => {
    const a = await Client.open();
    let view;
    try {
      const r = await a.request({ op: 'ui' });
      assert.equal(r.ok, true, JSON.stringify(r));
      assert.equal(typeof r.view, 'string');
      assert.match(r.view, /^screen \S+/);
      view = r.view;
    } finally {
      await a.close();
    }
    const ref = view.match(/\[(e\d+)\]/)?.[1];
    const b = await Client.open();
    try {
      if (ref) {
        // Read-only: the ref, from a connection that never read the view.
        const r = await b.request({ op: 'ui', ref, detail: true });
        assert.equal(r.ok, true, JSON.stringify(r));
        assert.equal(r.node.ref, ref);
      }
      const stale = await b.request({ op: 'ui', action: 'click', ref: 'e999999' });
      assert.equal(stale.ok, false);
      assert.equal(stale.code, 'stale_ref');
      const json = await b.request({ op: 'ui', format: 'json', maxNodes: 5 });
      assert.equal(json.ok, true, JSON.stringify(json));
      assert.equal(typeof json.nodes, 'object');
    } finally {
      await b.close();
    }
  });
});
