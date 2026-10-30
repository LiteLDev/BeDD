# Plugging into a harness

`harness-shim.ps1` replaces the usual helpers of LeviLauncher harnesses (`Focus-Game`, `Assert-GameForeground`,
`Set-GameWindowSize`, `Move-Mouse`, `Click`, `Scroll`, `Press-Key`, `Type-Text`, `Get-ClientBitmap`, same parameters)
with versions that talk to the game over its control pipe, and wraps `Start-Game`/`Stop-Game`. Scenarios do not
change. Dot-source it right after the harness's own `lib.ps1`, at script level:

```powershell
. "$PSScriptRoot/lib.ps1"
if (-not $Foreground) {
    . "$beddDir\harness-shim.ps1"
    Register-BeDDNative ([MyE2E.Native])   # optional, see below
}
# after deploying your mod and moving the other mods aside:
if ($script:Headless) { Install-BeDDMod -ModsDir $script:ModsDir -PipeName 'BeDD-<instance>' }
# ... and in finally, once the game is closed:
if ($script:Headless) { Uninstall-BeDDMod -ModsDir $script:ModsDir }
```

`$beddDir` is where Be.D.D. is unpacked; the harness can [fetch it itself](/guide/installation#let-the-harness-fetch-it).
`Initialize-BeDDInstance [-Slot N]` returns the instance to use (`Dir`, `ModsDir`, `PipeName`) and
[sets up](/guide/new-pc) whatever the PC still lacks.

Start games with `Start-BeDDGame.ps1` rather than the launcher, and keep `BeDD` out of any "move other mods aside"
step: it is installed fresh each run and removed afterwards (a leftover copy would make a normally started game
ignore your mouse and keyboard).

## What the shim adds

| Function | |
|---|---|
| `Install-BeDDMod -ModsDir <dir> [-PipeName p] [-Placement back\|offscreen\|none] [-Trace] [-NoManager]` | copies the module into `<dir>\BeDD` with a `config.json` that turns background mode on (window size from `$script:WindowW`/`$script:WindowH`, default 1600×900), switches LeviLamina to its built-in crash logger and points the client at the pipe |
| `Uninstall-BeDDMod -ModsDir <dir>` | removes the module again, retrying while the game still holds the DLL. Call it in `finally` |
| `Set-WorldSinglePlayer <worldDir>` | makes a test world single-player (no multiplayer, LAN, Xbox Live or platform broadcast). Call it right after restoring a test world |
| `Register-BeDDNative <type>` | redirects the input methods of the harness's own native class (below) |
| `Start-Game` / `Stop-Game` | wrapped: the game holds the shared signaling lock (exclusive with `$script:ExclusiveSignaling = $true`) while it runs, the manager window is started unless it runs, and the foreground goes back to where you were if the game takes it while starting |

## A harness's own input class

If your scenarios call the harness's own input class directly (`[MyE2E.Native]::Key(0x10, $false)` and friends),
give it these fields and a one-line guard at the top of each method; `Register-BeDDNative` fills them:

```csharp
public static Action<ushort, bool> KeyRedirect;          // (vk, up)
public static Action<int, bool>    MouseButtonRedirect;  // (0 left / 1 right / 2 middle, up)
public static Action<int>          WheelRedirect;        // (delta, 120 per notch)
public static Action<char>         UnicodeRedirect;
public static Action<int, int>     MoveRelativeRedirect; // camera
public static void Key(ushort vk, bool up) { if (KeyRedirect != null) { KeyRedirect(vk, up); return; } /* SendInput as before */ }
```

## Examples

[BeetrootSkin](https://github.com/LiteLDev/BeetrootSkin/tree/main/tools/e2e) and
[AlleviatedBackpacks](https://github.com/LiteLDev/AlleviatedBackpacks/tree/main/tools/e2e) have complete harnesses to
copy from.
