# Introduction

Be.D.D. runs end-to-end tests of your [LeviLamina](https://github.com/LiteLDev/LeviLamina) client mods
**in the background**. Your script moves the player, opens menus, presses keys and clicks inside Minecraft Bedrock,
while your own mouse, keyboard and windows stay yours, and several games can play at once.

![Your input goes to your apps; the script drives three background games over one WebSocket each](/showcase/how-it-feels.svg)

::: warning A testing tool
A game with Be.D.D. in its `mods\` folder ignores your real mouse and keyboard and shows up only in the manager
window. Be.D.D. puts itself only into its own clones of your game instance, never into the instance you play.
:::

## What is in it

Be.D.D. is a testing toolkit, not a mod for players. A release (`bedd-windows-x64.zip`) holds:

| Part | |
|---|---|
| `BeDD.exe` | one app for everything: double-click it for the [manager window](/guide/several-games#the-manager-window), or run it with arguments as the command line that clones, sets up and starts the games ([Driving the game](/guide/usage)) |
| `mod\BeDD\` | the client-side LeviLamina mod that `BeDD.exe mod install` puts into a game instance |
| `mod\BeDD-server\` | the same for a Bedrock Dedicated Server, for [client + server runs](/guide/modes) |
| `spec\bedd.asyncapi.yaml` | the AsyncAPI description of the WebSocket protocol, to generate a client from ([WebSocket protocol](/reference/websocket)) |

Your tests talk to each game over a **WebSocket** on `127.0.0.1`, from any language: Node, Python, C#, PowerShell,
or [bedboo](https://github.com/LiteLDev/bedboo)'s mineflayer API.

Only the test PC's game needs it; servers and players do not. Be.D.D. is not a lip package and is not listed on
LeviMC Mods: you download it from the [releases](https://github.com/LiteLDev/BeDD/releases).

## Features

- **Keep using your PC.** The game never takes the foreground, the cursor or your keystrokes, and your clicks never
  land in it.
- **Three ways to drive it, cheapest first.** Game-level ops for [bedboo](https://github.com/LiteLDev/bedboo) (walk,
  look, dig, use, chat), a UI tree of the menus to click by name, and simulated keyboard, mouse and screenshots as
  the last resort ([Operation modes](/guide/operation-modes)).
- **Any language.** One JSON message per request over a local WebSocket, described by an AsyncAPI spec.
- **Several games at once.** Each game has its own WebSocket; a cheap clone of an instance takes a few seconds.
- **One window for all of them.** The games of every test run on the PC show up live in one manager window instead
  of popping up on your desktop; closing it closes them all.
- **Plays like a player.** Keys, chat, key bindings, menus, hover tooltips, clicks and the camera all work as with
  a real mouse and keyboard.
- **Deterministic.** Every request returns once the game has handled it, so no guessing sleeps.
- **Your instance stays as it is.** Be.D.D. reads LeviLauncher's instances and never writes into them.

## Requirements

- Windows 10 or 11, x64.
- [LeviLauncher](https://docs.levimc.org/LeviLauncher/) **1.1** or newer, with a Minecraft Bedrock Edition
  **1.26.60** (GDK, release) instance that has LeviLamina **26.60.x**.

Nothing else: no PowerShell, no lip and no .NET runtime for clients. On a PC that has none of this yet, see
[Setting up a new PC](/guide/new-pc).

## The name

Be.D.D. (Bedrock Deep Dive) is named after the H.D.D. of Zenless Zone Zero: a Proxy guides agents through the
Hollows from afar, your script guides Minecraft from the background.

## Next

[Install it](/guide/installation), then [drive a game](/guide/usage).
