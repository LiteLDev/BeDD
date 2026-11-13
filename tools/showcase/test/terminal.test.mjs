import { describe, it } from 'node:test'
import assert from 'node:assert/strict'
import { wrap, renderAss } from '../lib/terminal.mjs'
import { pickRef } from '../lib/ui.mjs'
import { sourceOf } from '../demo.mjs'

const flat = row => row.map(p => p.text).join('')

describe('terminal pane', () => {
  it('wraps at spaces, indents continuation rows and keeps colours', () => {
    const rows = wrap([{ text: '[1] ', color: 'cyan' }, { text: 'aaa bbb ccc ddd', color: 'default' }], 12, 2)
    assert.deepEqual(rows.map(flat), ['[1] aaa bbb ', '  ccc ddd'])
    assert.equal(rows[0][0].color, 'cyan')
    assert.equal(rows[1].at(-1).color, 'default')
  })

  it('cuts a word longer than a row', () => {
    assert.deepEqual(wrap([{ text: 'abcdefghij' }], 4, 1).map(flat), ['abcd', ' efg', ' hij'])
  })

  it('escapes ASS overrides and times lines from t0', () => {
    const ass = renderAss([{ t: 1500, parts: [{ text: "g.call('look', { yaw: 1 }) \\n", color: 'green' }] }],
      { t0: 1000, durationMs: 3000 })
    const line = ass.split('\n').find(l => l.startsWith('Dialogue:'))
    assert.match(line, /^Dialogue: 0,0:00:00\.50,0:00:03\.00,T,/)
    assert.ok(line.includes('\\{ yaw: 1 \\}'))
    assert.ok(line.includes('{\\c&H0CC616&}'))
    assert.ok(!line.includes(' \\n'))
  })
})

describe('demo / ui helpers', () => {
  it('prints a step as its source', () => {
    assert.equal(sourceOf(g => g.call('key', { key: 'ESC' })), "g.call('key', { key: 'ESC' })")
    assert.equal(sourceOf(async g => { for (const y of [1]) await g.call('look', { yaw: y }) }),
      "for (const y of [1]) await g.call('look', { yaw: y })")
  })

  it('picks a visible, clickable node by name or text', () => {
    const nodes = {
      a: { role: 'text', name: 'Play' },
      b: { role: 'button', name: 'Play', actions: ['click'] },
      c: { role: 'button', name: 'Play', actions: ['click'], state: { visible: false } },
      d: { role: 'button', text: 'Cancel', actions: ['click'] }
    }
    assert.equal(pickRef(nodes, /^play$/i), 'b')
    assert.equal(pickRef(nodes, 'cancel'), 'd')
    assert.equal(pickRef(nodes, 'settings'), null)
  })
})
