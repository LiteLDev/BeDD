---
layout: home

hero:
  name: Be.D.D.
  text: Minecraft tests that run in the background
  tagline: Your script presses keys, types and clicks inside Minecraft Bedrock, while your own mouse, keyboard and windows stay yours, and several games play at once.
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
  - title: Several games at once
    details: Each game instance has its own pipe; a cheap clone of an instance takes a few seconds.
    link: /guide/several-games
  - title: One window for all of them
    details: The games of every test run on the PC show up live in one manager window instead of popping up on your desktop; closing it closes them all.
    link: /guide/several-games#the-manager-window
  - title: Plays like a player
    details: Keys, chat, key bindings, menus, hover tooltips, clicks and the camera all work as with a real mouse and keyboard.
    link: /guide/usage
  - title: Deterministic
    details: Every call returns once the game has handled the input, so no guessing sleeps. Screenshots work while the window is covered.
    link: /reference/how-it-works
  - title: Drop-in for existing harnesses
    details: One file replaces the usual Click, Press-Key and Focus-Game helpers of LeviLauncher harnesses; scenarios stay as they are.
    link: /guide/harness
---

![Three headless Minecraft games react to a test script in the manager window](/showcase/desktop.webp)

Be.D.D. (Bedrock Deep Dive) is named after the H.D.D. of Zenless Zone Zero: a Proxy guides agents through the
Hollows from afar, your script guides Minecraft from the background.

It is a testing toolkit for end-to-end tests of [LeviLamina](https://github.com/LiteLDev/LeviLamina) client mods,
not a mod for players: a client-side LeviLamina module that your test harness puts into a game instance for a run,
the PowerShell client that drives it, and the scripts that set up, clone and start the games.

```powershell
Import-Module .\mods\BeDD\BeDD.psm1
Connect-BeDD                               # waits until the game is up
Send-BeDDKey E                             # open the inventory
Send-BeDDMouse click -RX 0.5 -RY 0.474     # click at a fraction of the window
Send-BeDDKey T; Send-BeDDText '/give @s apple 3'; Send-BeDDKey ENTER
Save-BeDDScreenshot C:\temp\shot.png       # works while the window is covered
```

::: tip Start here
Read the [introduction](/guide/introduction), then [install](/guide/installation) it and
[drive your first game](/guide/usage).
:::
