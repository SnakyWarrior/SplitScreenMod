# SplitScreenMod

Local split-screen for **Minecraft Bedrock on Windows**: up to 4 players on one PC, each with their own controller.

Minecraft Bedrock already contains split-screen (it is used on Xbox), but the Windows version switches it off. This [LeviLamina](https://github.com/LiteLDev/LeviLamina) mod switches it back on.

![Two players in split-screen on PC](docs/screenshot.png)

## Features

- Press **F8** in a world to add a player. The screen splits, just like on console.
- Every controller gets its own player. Player 1 can keep using keyboard and mouse.
- Nothing else in the game is changed. The mod only answers "yes" where the game asks whether split-screen is allowed for joining and for assigning controllers.
- It finds those places by the shape of the game code, not by fixed addresses, so a new game version normally needs only a rebuild against the matching LeviLamina, not code changes.

## Requirements

| | |
|---|---|
| Game | Minecraft Bedrock for Windows (Microsoft Store / Xbox app) **1.26.60** |
| Mod loader | LeviLamina **26.60.x** (client). The easiest way to get it is through [LeviLauncher](https://github.com/LiteLDev/LeviLauncher) |
| Controllers | One per extra player. Xbox, PlayStation, Switch Pro and other gamepads that Windows supports |

## Installation

1. Install [LeviLauncher](https://github.com/LiteLDev/LeviLauncher) and create a game instance with LeviLamina.
2. Download `SplitScreenMod-client-windows-x64.zip` from the [latest release](../../releases/latest).
3. Extract the zip into a new folder `mods\SplitScreenMod\` inside your instance folder, for example `%AppData%\levilauncher.exe\versions\1.26.60.05\mods\SplitScreenMod\`. The folder must contain `manifest.json` and `SplitScreenMod.dll` directly, not inside another folder.
4. Start the game from LeviLauncher.

## How to play

1. Connect all controllers **before you start the game**, then start it.
2. Enter a world.
3. Press **F8** on the keyboard. Player 2 joins and the screen splits. Press F8 again for players 3 and 4 (tested with 2 players so far).
4. Pick up controller 2 and play.

If a controller still moves player 1, unplug it and plug it back in (or switch it off and on) while you are in the world.

## Troubleshooting

The mod writes a log to `mods\SplitScreenMod\splitscreen.log`. Please attach it when you [report a problem](../../issues/new/choose).

<details>
<summary>Advanced: splitscreen.cfg</summary>

For testing after a game update, you can put a `splitscreen.cfg` file next to the mod to override which callers get split-screen:

```
hook allowSplitScreen auto              # default
hook allowSplitScreen trace             # log every caller, change nothing
hook allowSplitScreen force 876204 ...  # allow only these call sites (offsets in Minecraft.Windows.exe)
```

</details>

## Building

You do not need Visual Studio. Every push is built by GitHub Actions; download the build from the Actions tab. To build locally, install [xmake](https://xmake.io) and run:

```
xmake f -a x64 -m release -p windows --target_type=client -y
xmake
```

## Licence

[GPL-3.0](LICENSE) © SnakyWarrior. This mod is not affiliated with Mojang or Microsoft.
