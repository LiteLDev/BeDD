---
layout: home

hero:
  name: Be.D.D.
  text: Minecraft tests that run in the background
  tagline: Your script moves the player, opens menus, presses keys and clicks inside Minecraft Bedrock, while your own mouse, keyboard and windows stay yours, and several games play at once.
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
    details: The game never takes the foreground, the cursor or your keystrokes, and your clicks never land in it.
    link: /guide/introduction
  - title: Three ways to drive it, cheapest first
    details: Game-level ops for bedboo's mineflayer API, a UI tree of the menus to click by name, and simulated keyboard, mouse and screenshots only as the last resort.
    link: /guide/operation-modes
  - title: Several games at once
    details: Each game has its own WebSocket; a cheap clone of an instance takes a few seconds.
    link: /guide/several-games
  - title: One window for all of them
    details: The games of every test run on the PC show up live in one manager window instead of popping up on your desktop; closing it closes them all.
    link: /guide/several-games#the-manager-window
  - title: Deterministic
    details: Every request returns once the game has handled it, so no guessing sleeps. Screenshots work while the window is covered.
    link: /reference/how-it-works
  - title: Any language
    details: One JSON message per request over a local WebSocket, described by an AsyncAPI spec. One app, BeDD.exe, clones, sets up and starts the games.
    link: /guide/integrating
---

![Three headless Minecraft games react to a test script in the manager window](/showcase/desktop.webp)

Be.D.D. (Bedrock Deep Dive) is named after the H.D.D. of Zenless Zone Zero: a Proxy guides agents through the
Hollows from afar, your script guides Minecraft from the background.

It is a testing toolkit for end-to-end tests of [LeviLamina](https://github.com/LiteLDev/LeviLamina) client mods,
not a mod for players: a client-side LeviLamina mod that serves a WebSocket, and `BeDD.exe`, the app that clones,
sets up and starts the games and shows them all in one window.

```powershell
BeDD.exe launch --slot 1 --detach --json   # {"pid":…,"url":"ws://127.0.0.1:53021/v1","token":"…"}
```

```js
const ws = new WebSocket(`${url}?token=${token}`);                       // Node 24; any language works
ws.onopen = () => {
  ws.send(JSON.stringify({ id: 1, op: 'ui' }));                          // the menu as named nodes
  ws.send(JSON.stringify({ id: 2, op: 'input', move: { x: 0, z: 1 } })); // walk forward, in a world
};
ws.onmessage = (e) => console.log(e.data);
```

::: tip Start here
Read the [introduction](/guide/introduction), then [install](/guide/installation) it and
[drive your first game](/guide/usage).
:::
