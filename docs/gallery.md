# Gallery

Recorded from the real screen with `tools/showcase/record.ps1` ([how](/reference/building#documentation-and-showcase-media)).

## Three games, one desktop, your PC stays yours

<video controls muted playsinline preload="none" poster="/BeDD/showcase/desktop-poster.webp" src="/BeDD/showcase/desktop.mp4" width="1280" height="720" style="width:100%;height:auto"></video>

A test script drives three Minecraft instances at once while the terminal in front keeps the focus. Recorded from
the real screen.

## Every line, live in the game

<video controls muted playsinline preload="none" poster="/BeDD/showcase/script-and-game-poster.webp" src="/BeDD/showcase/script-and-game.mp4" width="1280" height="360" style="width:100%;height:auto"></video>

`Send-BeDDKey E` opens the inventory, `Send-BeDDMouse` hovers and moves the sword, `ESC` closes it, in a window you
never clicked.

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

## The games never take the focus

![Three games and a terminal; only the terminal's title bar is active](/showcase/desktop-poster.webp)

Look at the title bars: only the terminal is active. Each game runs from its own cheap clone of the instance.
