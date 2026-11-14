// ui mode helpers: find a control in the game's UI tree by its name or text and act on it by ref
// (docs/reference/protocol.md, op `ui`). JSON UI screens are read control by control; an OreUI
// screen is one opaque node, so for those (and when the tree has no match) the caller passes the
// point the control sits at, as a fraction of the client area, and the helper falls back to it.

// Errors that mean "no tree to read here" (an off `ui` feature, an older mod, a screen still
// building), as opposed to a broken connection.
export const SOFT = new Set(['unsupported', 'disabled', 'not_ready', 'unknown_op'])

function matches (node, match) {
  const hay = [node.name, node.text, node.value].filter(v => typeof v === 'string' && v)
  if (match instanceof RegExp) return hay.some(s => match.test(s))
  const want = String(match).toLowerCase()
  return hay.some(s => s.toLowerCase() === want)
}

function hidden (node) {
  const s = node.state
  if (!s) return false
  if (Array.isArray(s)) return s.includes('hidden') || s.includes('disabled')
  return s.visible === false || s.hidden === true || s.disabled === true
}

// The best ref in a snapshot's nodes for match: visible, clickable first.
export function pickRef (nodes, match) {
  let best = null
  let bestScore = -1
  for (const [ref, node] of Object.entries(nodes ?? {})) {
    if (!node || hidden(node) || !matches(node, match)) continue
    const score = (node.actions?.includes?.('click') ? 2 : 0) + (node.role === 'button' ? 1 : 0)
    if (score > bestScore) { best = ref; bestScore = score }
  }
  return best
}

// { ref, snapshot } of the control, or the fallback point { rx, ry }. Both spread into a `mouse`
// request: v2's `mouse` takes a ref (it points at the control's centre; snapshot is ignored) as well as a point.
export async function find (g, match, fallback) {
  try {
    const snap = await g.call('ui', { format: 'json' })
    const ref = pickRef(snap.nodes, match)
    if (ref) return { ref, snapshot: snap.snapshot }
  } catch (e) {
    if (!SOFT.has(e.code)) throw e
  }
  if (!fallback) throw new Error(`no control matches ${match} on the game's screen`)
  return { ...fallback }
}

// Clicks the control: `ui` click by ref, else a `mouse` click at the fallback point (OreUI: there
// is nothing semantic to click). A tree that changed between read and click is read again.
export async function click (g, match, fallback) {
  for (let attempt = 0; ; attempt++) {
    const at = await find(g, match, fallback)
    if (!at.ref) return g.call('mouse', { action: 'click', rx: at.rx, ry: at.ry })
    try {
      return await g.call('ui', { snapshot: at.snapshot, action: 'click', ref: at.ref })
    } catch (e) {
      if (attempt >= 2 || !['stale_ref', 'stale_snapshot'].includes(e.code)) throw e
    }
  }
}
