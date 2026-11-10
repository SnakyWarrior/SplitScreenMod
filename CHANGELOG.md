# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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

[Unreleased]: https://github.com/SnakyWarrior/SplitScreenMod/compare/v1.2.0...HEAD
[1.2.0]: https://github.com/SnakyWarrior/SplitScreenMod/compare/v1.1.1...v1.2.0
[1.1.1]: https://github.com/SnakyWarrior/SplitScreenMod/compare/v1.1.0...v1.1.1
[1.1.0]: https://github.com/SnakyWarrior/SplitScreenMod/compare/v1.0.1...v1.1.0
[1.0.1]: https://github.com/SnakyWarrior/SplitScreenMod/compare/v1.0.0...v1.0.1
[1.0.0]: https://github.com/SnakyWarrior/SplitScreenMod/releases/tag/v1.0.0
