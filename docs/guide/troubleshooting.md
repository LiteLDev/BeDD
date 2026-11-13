# Troubleshooting

## Connecting fails with 401 or 403

- **401**: the token is missing or wrong. Take `url` and `token` from the same `run\<pid>.json` (or
  `BeDD.exe ps --json`): a game gets a new token every time it starts, so a token saved from an earlier run no
  longer works.
- **403**: the request carried an `Origin` header, which browsers and some WebSocket libraries add. Leave it out, or
  list that origin in the config's `allowOrigins`.
- **Connection refused**: the game is not up yet, or has exited. `launch --detach` returns only once the WebSocket
  listens; otherwise wait for `run\<pid>.json` to appear.
- **Too many connections**: the game serves `wsMaxClients` (8) connections at once. Close the ones you no longer
  need.

## The game closes by itself after about 30 s

No manager window attached to it within 30 s (or it lost its manager), so it closed; its `logs\latest.log` says why.
`BeDD.exe launch` opens the manager window when none runs: check `BeDD.exe manager status`, and
`%LOCALAPPDATA%\BeDD\logs\manager.log`. For a headless CI run that does not use the window, set `manager` to `false`
(`BeDD.exe mod config <instance> --set manager=false`): the game then stays hidden and never closes by itself.

## Closing the manager window closed all games

That is what it does (it asks first): the games of every run on the PC are in it, and a game never runs without
it.

## A clone takes minutes and gigabytes

The clone is on another volume than the base instance, so its files were copied instead of hardlinked. Set
`BEDD_INSTANCES_DIR` to a folder on the base's drive (the base is under `%LOCALAPPDATA%\LeviLauncher\Instances`),
then `BeDD.exe instance remove` the copied clone and make it again. See [Clones](/guide/new-pc#clones).

## BeDD.exe cannot find LeviLauncher or the instance

- **No LeviLauncher, or one older than 1.1**: older versions have no command line for Be.D.D. to ask. Update
  LeviLauncher, or point `BEDD_LEVILAUNCHER` at a 1.1+ `LeviLauncher.exe`.
- **No 1.26.60 instance**: `LeviLauncher.exe instance list` shows what it has; make one with
  `LeviLauncher.exe install 1.26.60 --loader`. With several, pick one with `BEDD_INSTANCE` or `--base`.
- `BeDD.exe instance list --all` shows what Be.D.D. sees.

## LeviLamina version mismatch

The mod is built for Minecraft **1.26.60** with LeviLamina **26.60.x**. With another game version or LeviLamina the
mod does not load (LeviLamina logs why in the clone's `logs\latest.log`) and no WebSocket ever appears. Update the base
instance in LeviLauncher (`LeviLauncher.exe loader install`), then `BeDD.exe instance refresh <instance>` so the
clone follows it.

## "An error has occurred" (code "Blaze") with several games

Games on one PC share an online sign-in, and multiplayer worlds opened at the same time fail. Make test worlds
single-player with `BeDD.exe world singleplayer <worldDir>`. A run that must join a server online uses
`BeDD.exe launch --exclusive-signaling` and gets the PC to itself while the other games close. See
[Rules for parallel runs](/guide/several-games#rules-for-parallel-runs).

## The game uses my normal Minecraft worlds and options

The game uses the instance's own data only when it is started as `Minecraft.Windows.exe` from the clone, with
LeviLauncher's loader in the exe. Start it with `BeDD.exe launch`, and make sure the base instance was installed by
LeviLauncher 1.1 or newer (it adds the loader).

## A Windows Firewall prompt at every new clone

With `loopbackOnly` (on by default) the game stays on the PC and asks for nothing. Dedicated servers you start
without BeDD's server mod still ask once per server folder; **Cancel** is fine.

## A console window flashes when the game starts

That is LeviLamina's `CrashLogger.exe`. `BeDD.exe mod install` and `launch` switch the clone to the built-in crash
logger; for an instance set up another way, set `modules.crashLogger.builtin` in
`mods\LeviLamina\config\Config.json`.

## I closed someone else's game

Find your game by its **instance** (`BeDD.exe ps --json`), never just by the process name `Minecraft.Windows`, and
give every parallel run its own slot.

## `look` times out, or game ops do nothing after pause

A paused singleplayer world does not tick, and `look`, `slot`, `action` and `button` are applied on client ticks.
After `action pause` there, resume with `key ESC`.

## `ui` answers `stale_snapshot`, `stale_ref` or `not_ready`

The screen changed since the snapshot the `ref` came from, or it is still building. Read `ui` again and use the new
`snapshot` and `ref`. An Ore UI screen shows as one opaque node: drive it with [simulated input](/guide/operation-modes#simulated-input).

## An op answers `code:"disabled"`

That op's [feature key](/guide/configuration#feature-keys) is `false` in this clone's config: `gameHooks`, `tap`,
`allowSend`, `allowStack` or `ui`. An op the build lacks answers `unknown op`.

## Reporting a bug

Open an issue on [GitHub](https://github.com/LiteLDev/BeDD/issues/new/choose) with:

- the Be.D.D., LeviLamina and Minecraft versions (`BeDD.exe version`);
- what the script sent (the JSON request) and what the game did instead;
- `mods\BeDD\config\config.json` (`BeDD.exe mod config <instance>`);
- the relevant part of `logs\latest.log`, ideally with `"trace": true` (it shows which input reached the game and
  whether it was synthetic or real).
