# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- The menus follow the game's language (16 languages, including Chinese, Japanese and Korean).

### Removed

- The F7 shortcut (it clashed with other mods' keys); the Split screen menu opens from the pause menu.

### Fixed

- After taking player 1's controller away, the game's "Join as Player" prompt did not show.

## [1.3.0] - 2026-11-10

### Added

- Alone in a world with a controller, the pause menu shows a red controller button instead of the Split screen button: it takes player 1's controller away so it can choose another player.
- Open Split screen menus refresh for every player when the layout, the players or their controllers change.
- The log of the run before is kept as `splitscreen.prev.log`, so a crash can still be reported after the game restarts.
- A Patreon link, for anyone who wants to support the mod (it stays free).

### Changed

- The Split screen button only shows with several players.
- Swapping players no longer shows "left the game" / "joined the game" in chat.
- The world no longer tries to pause when every player is in the pause menu (the game does not allow it with several players).
- Built against the newest LeviLamina 26.60, falling back to 26.60.5.

### Fixed

- The game froze when a player opened the pause menu with two or more players.
- The game crashed when a player left after the pause menu had been closed by the mod.
- The game hung when a controller pressed start while the world was paused.
- Pressing the Split screen button as player 2 opened player 1's menu.
- After taking player 1's controller away, player 1 kept walking or turning by itself.
- A player could stay in spectator mode after choosing who plays.
- A new player that the game dropped at once is joined again.
- The pause menu buttons did nothing until the pause menu was closed in a one-player world.

## [1.2.0] - 2026-11-10

### Added

- Choose who plays: press start on a free controller and the screen splits, with the new player's part black and a menu until you choose a saved local player (each with their own inventory), player 1, or a player who lost their controller. The menu closes by itself after 10 seconds.
- A Split screen menu, opened by the name tag button in the pause menu (next to the small buttons under Save & Quit) or F7: choose the layout (top and bottom or side by side), take another player's place, remove players, and take player 1's controller away so it can choose another player.
- Menus in the style of the game's new UI, with P1-P4 pictures in each player's own colour and status tags (Player 1, Playing, No controller, Not playing, You).
- The world pauses when every player is in the pause menu.
- A short message above the hotbar after each choice.

### Changed

- In a world, controllers no longer go to player 1 by themselves; a controller that is already connected when you enter a world waits for start to be pressed.
- Controllers ask who is playing again in every world instead of keeping their player from the last one.
- While a new player is being chosen, the player that holds their part of the screen waits in spectator mode, and its "joined", "left" and appearance chat lines are hidden.
- Built against LeviLamina 26.60.5.

### Fixed

- Leaving and re-entering a world after playing split-screen crashed (LeviLamina kept a pointer to a player that had left).
- Reconnecting controllers could open more than four controller slots and crash.
- A new player who joined on a controller slot unplugged long before was removed at once.

## [1.1.1] - 2026-11-09

### Added

- The "Controller lost connection" prompt shows how many seconds are left before that player is removed.

### Changed

- F8 no longer adds a player: a player without a controller could not be removed.

### Fixed

- The game crashed when a player without a controller was removed after 20 seconds.

## [1.1.0] - 2026-11-09

### Added

- Press a button on a free controller to join (the game's own join prompt), as on Xbox.
- A player whose controller stays disconnected for 20 seconds is removed.
- F9 writes the controller-to-player table to the log.

### Fixed

- Controllers that also report mouse or keyboard input (e.g. 8BitDo, GameSir) were all given to player 1.
- A reconnected controller takes back the free player instead of being ignored.
- F8 adds the player on the first free controller slot.

## [1.0.1] - 2026-11-09

### Fixed

- One F8 press could add several players at once.

## [1.0.0] - 2026-11-09

### Added

- Local split-screen on Minecraft Bedrock for Windows 1.26.60 (LeviLamina 26.60 client).
- F8 adds a player (up to 4).
- Each controller gets its own player.
- Call sites are detected by code pattern, not fixed addresses.

[Unreleased]: https://github.com/SnakyWarrior/SplitScreenMod/compare/v1.3.0...HEAD
[1.3.0]: https://github.com/SnakyWarrior/SplitScreenMod/compare/v1.2.0...v1.3.0
[1.2.0]: https://github.com/SnakyWarrior/SplitScreenMod/compare/v1.1.1...v1.2.0
[1.1.1]: https://github.com/SnakyWarrior/SplitScreenMod/compare/v1.1.0...v1.1.1
[1.1.0]: https://github.com/SnakyWarrior/SplitScreenMod/compare/v1.0.1...v1.1.0
[1.0.1]: https://github.com/SnakyWarrior/SplitScreenMod/compare/v1.0.0...v1.0.1
[1.0.0]: https://github.com/SnakyWarrior/SplitScreenMod/releases/tag/v1.0.0
