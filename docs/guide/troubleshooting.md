# Troubleshooting

## My game ignores my mouse and keyboard

The module is in the instance's `mods\` folder, often a copy a harness left behind: the game then runs headless and
shows up only in the manager window. Close it there, then delete `mods\BeDD`. Harnesses call `Uninstall-BeDDMod` in
`finally` so this does not happen, and keep `BeDD` out of their "move other mods aside" step.

## "An error has occurred" (code "Blaze") with several games

Games on one PC share an online sign-in, and multiplayer worlds opened at the same time fail. Make test worlds
single-player with `Set-WorldSinglePlayer <worldDir>`. A run that must join a server online sets
`$script:ExclusiveSignaling = $true` before `Start-Game` and gets the PC to itself while the other games close. See
[Rules for parallel runs](/guide/several-games#rules-for-parallel-runs).

## The game uses my normal Minecraft worlds and options

The game uses the instance's own data only when it is started as `Minecraft.Windows.exe` and LeviLauncher's loader
is in the exe. Start it with `Start-BeDDGame.ps1`, which does both: [setup](/guide/new-pc) adds the loader by
starting the instance once through LeviLauncher.

## A Windows Firewall prompt at every new instance

With `loopbackOnly` (on by default) the game stays on the PC and asks for nothing. Dedicated servers your harness
starts still ask once per server folder; **Cancel** is fine.

## A console window flashes when the game starts

That is LeviLamina's `CrashLogger.exe`. `Install-BeDDMod` and `Start-BeDDGame.ps1` switch the instance to the
built-in crash logger; for an instance set up another way, set `modules.crashLogger.builtin` in
`mods\LeviLamina\config\Config.json`.

## I closed someone else's game

Find your game by its **instance folder**, never just by the process name `Minecraft.Windows`, and give every
instance its own lock file.

## Closing the manager window closed all games

That is what it does (it asks first): the games of every run on the PC are in it, and a game never runs without
it.

## The game closes by itself while starting

No manager window linked to it within 90 s (or it lost its manager), so it closed; its `logs\latest.log` says why.
The game starts the manager with the `managerScript` of its config when none runs: check that the path is right
(`Install-BeDDMod` writes it) and that PowerShell 7 (`pwsh`) is installed. `%LOCALAPPDATA%\BeDD\manager.log` is the
manager's own log.

## `Set-BeDDLook` times out, or game ops do nothing after pause

A paused singleplayer world does not tick, and `look`, `slot`, `action` and `button` are applied on client ticks.
After `Invoke-BeDDAction pause` there, resume with `Send-BeDDKey ESC`.

## An op answers `code:"disabled"`

That op needs a config key: `gameHooks`, `tap`, `allowSend` or `allowStack` ([Configuration](/guide/configuration#keys-for-bedboo)).
An op the build lacks answers `unknown op`.

## Reporting a bug

Open an issue on [GitHub](https://github.com/LiteLDev/BeDD/issues/new/choose) with:

- the Be.D.D., LeviLamina and Minecraft versions;
- what the script sent (the JSON request or the PowerShell call) and what the game did instead;
- `mods/BeDD/config/config.json`;
- the relevant part of `logs/latest.log`, ideally with `"trace": true` (it shows which input reached the game and
  whether it was synthetic or real).
