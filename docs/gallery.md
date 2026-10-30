# Gallery

Recorded with `tools/showcase/record.ps1`, straight from the manager window and the terminal ([how](/reference/building#documentation-and-showcase-media)).

## Three headless games, one manager window

<video controls muted playsinline preload="none" poster="/BeDD/showcase/desktop-poster.webp" src="/BeDD/showcase/desktop.mp4" width="1280" height="720" style="width:100%;height:auto"></video>

A test script drives three headless Minecraft instances at once. The games have no windows of their own: the
manager window shows each one live, with its instance, pid and whether input is arriving.

## Every line, live in the game

<video controls muted playsinline preload="none" poster="/BeDD/showcase/script-and-game-poster.webp" src="/BeDD/showcase/script-and-game.mp4" width="1020" height="360" style="width:100%;height:auto"></video>

`Send-BeDDKey E` opens the inventory, `Send-BeDDMouse` hovers and moves the sword, `ESC` closes it, in a game you
never touched.

## Your input stays yours

![Your input goes to your apps; the script drives three background games over one pipe each](/showcase/how-it-feels.svg)

Your mouse and keyboard go to your apps; the script reaches each game over its own pipe and gets an answer once the
game has handled the input.

## Chat commands, typed for you

![Send-BeDDText types '/summon sheep' and '/summon cow' into chat](/showcase/chat-summon.webp)

`Send-BeDDText` types `/summon sheep` and `/summon cow` into chat; the game runs them like a player typed them.

## Hover and click where you aim

![A virtual cursor brings up a tooltip in the inventory](/showcase/inventory-hover.webp)

A virtual cursor brings up tooltips and clicks slots; this picture is `Save-BeDDScreenshot` of a window that was off
the screen.

## One window for every game

![The manager window with three headless games next to the terminal running the script](/showcase/desktop-poster.webp)

No game windows, no taskbar buttons, nothing takes the focus: the games of every run show up in the manager window,
and closing it closes them all. Each game runs from its own cheap clone of the instance.
