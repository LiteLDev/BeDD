// An in-process stand-in for the mod's WebSocket server: node:http upgrade on 127.0.0.1, path /v1,
// the token as ?token= or "Authorization: Bearer", and the RFC 6455 subset the client needs (text,
// binary, ping/pong, close, continuation frames; client frames masked, server frames not).

import { createServer } from 'node:http'
import { createHash } from 'node:crypto'

const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11'

export function encodeFrame (opcode, payload = Buffer.alloc(0)) {
  const len = payload.length
  let head
  if (len < 126) {
    head = Buffer.from([0x80 | opcode, len])
  } else if (len < 65536) {
    head = Buffer.alloc(4)
    head[0] = 0x80 | opcode; head[1] = 126; head.writeUInt16BE(len, 2)
  } else {
    head = Buffer.alloc(10)
    head[0] = 0x80 | opcode; head[1] = 127; head.writeBigUInt64BE(BigInt(len), 2)
  }
  return Buffer.concat([head, payload])
}

// Splits buf into complete frames; returns { frames: [{fin, opcode, payload}], rest }.
export function decodeFrames (buf) {
  const frames = []
  let off = 0
  while (buf.length - off >= 2) {
    const b0 = buf[off]; const b1 = buf[off + 1]
    let len = b1 & 0x7f
    let p = off + 2
    if (len === 126) {
      if (buf.length - p < 2) break
      len = buf.readUInt16BE(p); p += 2
    } else if (len === 127) {
      if (buf.length - p < 8) break
      len = Number(buf.readBigUInt64BE(p)); p += 8
    }
    const masked = (b1 & 0x80) !== 0
    if (masked) {
      if (buf.length - p < 4) break
    }
    const mask = masked ? buf.subarray(p, p + 4) : null
    if (masked) p += 4
    if (buf.length - p < len) break
    const payload = Buffer.from(buf.subarray(p, p + len))
    if (mask) for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i & 3]
    frames.push({ fin: (b0 & 0x80) !== 0, opcode: b0 & 0x0f, payload })
    off = p + len
  }
  return { frames, rest: buf.subarray(off) }
}

// handler(msg, conn) -> reply object | undefined (no reply) | Promise of either. The reply gets the
// request's id unless it sets one.
export async function startFakeServer ({ token = 'a'.repeat(64), port = 0, handler = () => ({ ok: true }) } = {}) {
  const connections = new Set()
  const requests = []
  const server = createServer((req, res) => { res.writeHead(426); res.end() })

  server.on('upgrade', (req, socket) => {
    const url = new URL(req.url, 'http://127.0.0.1')
    const auth = req.headers.authorization
    const given = url.searchParams.get('token') ?? (auth?.startsWith('Bearer ') ? auth.slice(7) : null)
    const reject = status => {
      socket.end(`HTTP/1.1 ${status} ${status === 401 ? 'Unauthorized' : 'Not Found'}\r\nContent-Length: 0\r\nConnection: close\r\n\r\n`)
    }
    if (url.pathname !== '/v1') return reject(404)
    if (given !== token) return reject(401)
    const accept = createHash('sha1').update(req.headers['sec-websocket-key'] + GUID).digest('base64')
    socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
      `Sec-WebSocket-Accept: ${accept}\r\n\r\n`)

    const conn = {
      socket,
      sendText: s => socket.write(encodeFrame(1, Buffer.from(s))),
      sendBinary: b => socket.write(encodeFrame(2, Buffer.from(b))),
      close: () => { socket.write(encodeFrame(8, Buffer.from([0x03, 0xe8]))); socket.end() }
    }
    connections.add(conn)
    socket.on('close', () => connections.delete(conn))
    socket.on('error', () => {})

    let pending = Buffer.alloc(0)
    let message = []
    let messageOp = 0
    socket.on('data', chunk => {
      const { frames, rest } = decodeFrames(Buffer.concat([pending, chunk]))
      pending = Buffer.from(rest)
      for (const f of frames) {
        if (f.opcode === 8) { conn.close(); return }
        if (f.opcode === 9) { socket.write(encodeFrame(10, f.payload)); continue }
        if (f.opcode === 10) continue
        if (f.opcode !== 0) messageOp = f.opcode
        message.push(f.payload)
        if (!f.fin) continue
        const data = Buffer.concat(message)
        message = []
        if (messageOp !== 1) continue
        const msg = JSON.parse(data.toString('utf8'))
        requests.push(msg)
        Promise.resolve(handler(msg, conn)).then(reply => {
          if (reply === undefined || socket.destroyed) return
          conn.sendText(JSON.stringify({ id: msg.id, ...reply }))
        })
      }
    })
  })

  await new Promise(resolve => server.listen(port, '127.0.0.1', resolve))
  return {
    url: `ws://127.0.0.1:${server.address().port}/v1`,
    token,
    requests,
    connections,
    async close () {
      for (const c of connections) c.socket.destroy()
      await new Promise(resolve => server.close(resolve))
    }
  }
}
