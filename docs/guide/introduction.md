# Introduction

Be.D.D. runs end-to-end tests of [LeviLamina](https://github.com/LiteLDev/LeviLamina) client mods in the background:
the game never takes the foreground, cursor or keystrokes, and several games run at once.

![Your input goes to your apps; the script drives three background games over one WebSocket each](/showcase/how-it-feels.svg)

::: warning A testing tool
A game with Be.D.D. in `mods\` ignores your real mouse and keyboard and shows only in the manager window. Be.D.D.
installs itself only into its own clones, never into the instance you play.
:::

## What is in it

| Release part | |
|---|---|
| `BeDD.exe` | double-click: the [manager window](/guide/several-games#the-manager-window); with arguments: the CLI that clones, sets up and starts games |
| `mod\BeDD\` | the client mod `BeDD.exe mod install` puts into an instance |
| `mod\BeDD-server\` | the server mod, for [client + server runs](/guide/modes) |
| `spec\bedd.asyncapi.yaml` | the [WebSocket protocol](/reference/websocket), for code generators |

Tests talk to each game over a WebSocket on `127.0.0.1`: through [bedboo](https://github.com/LiteLDev/bedboo) first,
or from any language ([Operation modes](/guide/operation-modes)). Only the test PC needs it. Not a lip package, not
on LeviMC Mods: get it from the [releases](https://github.com/LiteLDev/BeDD/releases).

Keys, chat, key bindings, menus, tooltips, clicks and the camera work as for a player. Every request returns once
handled.

## Requirements

- Windows 10 or 11, x64.
- [LeviLauncher](https://docs.levimc.org/LeviLauncher/) **1.1+** with a Minecraft Bedrock **1.26.60** (GDK, release)
  instance that has LeviLamina **26.60.x** ([Setting up a new PC](/guide/new-pc)).

No PowerShell, lip or .NET runtime needed.

## The name

Bedrock Deep Dive, after Zenless Zone Zero's H.D.D.: a Proxy guides agents through the Hollows from afar, your
script guides Minecraft from the background.
