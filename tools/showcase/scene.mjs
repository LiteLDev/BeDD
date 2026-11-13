// Per-take scene for record.mjs (imported again before every take, so it can be edited while
// record.mjs --keep-open waits): where each game stands and looks.
// game 1: the river, panning towards the trees (demo.mjs pans from yaw 0); game 2: the meadow
// (animals are summoned there); game 3: the forest behind the inventory.
export const spawn = '~ ~ ~'

// yaw / pitch in degrees, set with the bedboo `look` op.
export const facing = {
  1: { yaw: 0, pitch: 2 },
  2: { yaw: 90, pitch: 12 },
  3: { yaw: 180, pitch: 5 }
}

// What game 3 shows in its inventory (/give after /clear).
export const items = ['diamond_sword 1', 'golden_apple 4', 'cooked_beef 16', 'oak_log 32', 'torch 24', 'cake 1']
