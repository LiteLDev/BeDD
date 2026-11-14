---
layout: home

hero:
  name: Be.D.D.
  text: Minecraft tests that run in the background
  tagline: Your script plays Minecraft Bedrock while your mouse, keyboard and windows stay yours.
  image:
    src: /logo.png
    alt: Be.D.D.
  actions:
    - theme: brand
      text: Get started
      link: /guide/introduction
    - theme: alt
      text: Download
      link: https://github.com/LiteLDev/BeDD/releases/latest
    - theme: alt
      text: Gallery
      link: /gallery

features:
  - title: Keep using your PC
    details: The game never takes the foreground, the cursor or your keystrokes.
    link: /guide/introduction
  - title: Three ways to drive it, cheapest first
    details: bedboo's game ops, then menus as text, then simulated input.
    link: /guide/operation-modes
  - title: Several games at once
    details: One WebSocket per game; a clone of an instance takes seconds.
    link: /guide/several-games
  - title: One window for all of them
    details: Every game on the PC shows live in one manager window; closing it closes them all.
    link: /guide/several-games#the-manager-window
  - title: Deterministic
    details: Every request returns once the game has handled it.
    link: /reference/how-it-works
  - title: Any language
    details: JSON over a local WebSocket, with an AsyncAPI spec.
    link: /guide/integrating
---

![Three headless Minecraft games react to a test script in the manager window](/showcase/desktop.webp)

End-to-end tests for [LeviLamina](https://github.com/LiteLDev/LeviLamina) client mods: a client mod that serves a
WebSocket, and `BeDD.exe`, which clones, sets up and starts games.

```powershell
BeDD.exe launch --slot 1 --json   # {"pid":…,"url":"ws://127.0.0.1:53021/v1","token":"…",…}
```

```js
import bedboo from 'bedboo';
const bot = bedboo.createBot({ transport: 'bedd', bedd: { url, token } });
bot.once('spawn', () => bot.setControlState('forward', true));
```

Start: [introduction](/guide/introduction), [install](/guide/installation), [drive a game](/guide/usage).
