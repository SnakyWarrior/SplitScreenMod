<p align="center"><img src="docs/logo.png" alt="Bedrock Splitscreen" width="640"></p>

# SplitScreenMod

Local split-screen for **Minecraft Bedrock on Windows**: up to 4 players on one PC, each with their own controller.

Minecraft Bedrock already contains split-screen (it is used on Xbox), but the Windows version switches it off. This [LeviLamina](https://github.com/LiteLDev/LeviLamina) mod switches it back on.

![Two players in split-screen on PC](docs/screenshot.png)

## Features

- **Choose who plays.** Press start on a free controller: the screen splits and the new player's part stays black with a menu until you choose: play as player 1, as one of your saved local players (each keeps their own inventory), or take over a player whose controller was lost. Nobody joins until you choose.
- **Asks again in every world.** Controllers do not keep their player from the last world: after you enter a world, each controller asks who is playing.
- **One controller per player.** A controller that reconnects takes the place of a player who lost theirs and asks who should play.
- **Player 1 keeps keyboard and mouse.** In a world, a controller only controls player 1 when you pick player 1 in the menu. A controller that was already connected asks who it is for when you enter a world.
- **Players leave on their own:** a player (2 to 4) whose controller stays disconnected for 20 seconds is removed. The game's "Controller lost connection" prompt counts down the seconds.
- **Split screen menu:** a name tag button in the pause menu, next to the small buttons under Save & Quit (or **F7**) switches between top-and-bottom and side-by-side, and removes players.
- **Made to survive updates.** The mod finds the places it needs by the shape of the game code, not by fixed addresses, so a new game version normally needs only a rebuild against the matching LeviLamina.

## Requirements

| | |
|---|---|
| Game | Minecraft Bedrock for Windows (Microsoft Store / Xbox app) **1.26.60** |
| Mod loader | LeviLamina **26.60.x** (client). The easiest way to get it is through [LeviLauncher](https://github.com/LiteLDev/LeviLauncher) |
| Controllers | One per extra player. Xbox, PlayStation, Switch Pro and other gamepads that Windows supports |

## Installation

1. Install [LeviLauncher](https://github.com/LiteLDev/LeviLauncher) and create a game instance with LeviLamina.
2. Download `SplitScreenMod-client-windows-x64.zip` from the [latest release](../../releases/latest).
3. Extract the zip into a new folder `mods\SplitScreenMod\` inside your instance folder, for example `%LocalAppData%\LeviLauncher\Instances\1.26.60.05\mods\SplitScreenMod\` (older LeviLauncher versions use `%AppData%\levilauncher.exe\versions\...`). The folder must contain `manifest.json` and `SplitScreenMod.dll` directly, not inside another folder.
4. Start the game from LeviLauncher.

## How to play

1. Start the game and enter a world (with keyboard and mouse, or a controller).
2. Press start on another controller. Player 1 gets a menu in the game: pick who plays with that controller.
3. Repeat for players 3 and 4.
4. In the pause menu, choose the name tag button under Save & Quit (or press **F7**) to switch the split or remove a player.

To leave, remove the player in the Split screen menu, or unplug or switch off that controller: the "Controller lost connection" prompt counts down, and after 20 seconds the player is removed. Plug a controller back in before that to pick up a player again.

## Troubleshooting

The mod writes a log to `mods\SplitScreenMod\splitscreen.log`. If controllers go to the wrong player, press **F9** in the game first: it writes which controller belongs to which player into the log. Please attach the log when you [report a problem](../../issues/new/choose).

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

The menu colours and the cross icon come from [Spectrollay-OreUI](https://github.com/Spectrollay-OreUI/OreUI) (MIT, © 2020 Spectrollay). The Ore UI button, radio and dropdown textures come from [bedrock-core/ui](https://github.com/bedrock-core/ui) (MIT, © 2026 bedrock-core; licence in `resource_packs/SplitScreenUI/textures/ui/ore-styled/`).
