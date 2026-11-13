// The terminal pane of the showcase video. demo.mjs prints its lines to stdout and, with
// --transcript, also appends each one with its wall-clock time; renderAss turns that transcript into
// ASS subtitles that ffmpeg (libass) draws on a console-black picture. So the pane shows exactly
// what the script printed, when it printed it, without a console window to place and capture.

export const COLORS = {
  default: 'cccccc',
  gray: '767676',
  cyan: '61d6d6',
  yellow: 'f9f1a5',
  green: '16c60c',
  red: 'e74856'
}

const ANSI = { default: 0, gray: 90, cyan: 96, yellow: 93, green: 92, red: 91 }

// One printed line: parts [{ text, color }].
export function ansi (parts) {
  return parts.map(p => p.color && p.color !== 'default' ? `\x1b[${ANSI[p.color]}m${p.text}\x1b[0m` : p.text).join('')
}

function assTime (ms) {
  const cs = Math.max(0, Math.round(ms / 10))
  const h = Math.floor(cs / 360000)
  const m = Math.floor(cs / 6000) % 60
  const s = Math.floor(cs / 100) % 60
  return `${h}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}.${String(cs % 100).padStart(2, '0')}`
}

function assColor (name) {
  const hex = COLORS[name] ?? COLORS.default
  return `&H${hex.slice(4, 6)}${hex.slice(2, 4)}${hex.slice(0, 2)}&`.toUpperCase()
}

// Braces start override blocks and \n \N \h are line breaks / hard spaces in ASS.
function assText (s) {
  return s.replace(/\\(?=[nNh])/g, '\\​').replace(/[{}]/g, c => '\\' + c)
}

// Splits parts into rows of at most cols characters; continuation rows are indented by indent.
export function wrap (parts, cols, indent = 4) {
  const rows = []
  let row = []
  let used = 0
  const push = (text, color) => {
    if (!text) return
    row.push({ text, color })
    used += text.length
  }
  for (const { text, color } of parts) {
    let rest = text
    while (rest.length > cols - used) {
      const room = cols - used
      // Break after the last space that fits, else cut the word.
      const space = rest.lastIndexOf(' ', room)
      const cut = space >= 0 ? Math.min(space + 1, room) : room
      push(rest.slice(0, cut), color)
      rest = rest.slice(cut).replace(/^ +/, '')
      rows.push(row)
      row = [{ text: ' '.repeat(indent), color: 'default' }]
      used = indent
    }
    push(rest, color)
  }
  rows.push(row)
  return rows
}

// entries: [{ t (epoch ms), parts }]; t0: the epoch ms the video starts at.
export function renderAss (entries, { t0, durationMs, width = 672, height = 1080, fontSize = 18, margin = 16 }) {
  const lineH = Math.round(fontSize * 1.25)
  const cols = Math.floor((width - 2 * margin) / (fontSize * 0.55))
  const out = [
    '[Script Info]', 'ScriptType: v4.00+', `PlayResX: ${width}`, `PlayResY: ${height}`, 'WrapStyle: 2',
    'ScaledBorderAndShadow: yes', '',
    '[V4+ Styles]',
    'Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, ' +
      'Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, ' +
      'MarginR, MarginV, Encoding',
    `Style: T,Consolas,${fontSize},&H00CCCCCC,&H00CCCCCC,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,0,0,7,0,0,0,1`,
    '',
    '[Events]',
    'Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text'
  ]
  let row = 0
  const end = assTime(durationMs)
  for (const { t, parts } of entries) {
    for (const r of wrap(parts, cols)) {
      const y = margin + row * lineH
      row++
      if (y + lineH > height) continue // past the bottom of the pane
      // libass drops leading spaces: the indent of a continuation row is hard spaces.
      const text = r.map((p, i) => `{\\c${assColor(p.color)}}` +
        (i === 0 ? assText(p.text).replace(/^ +/, s => '\\h'.repeat(s.length)) : assText(p.text))).join('')
      out.push(`Dialogue: 0,${assTime(t - t0)},${end},T,,0,0,0,,{\\pos(${margin},${y})}${text}`)
    }
  }
  return out.join('\n') + '\n'
}
