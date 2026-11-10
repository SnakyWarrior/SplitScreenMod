# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- A menu in the game (on player 1's screen) asks who plays with a new controller before anyone joins: player 1, a saved local player with their own inventory, or a player who lost their controller.
- A name tag button in the pause menu, with the small buttons under Save & Quit (or F7), to switch between a horizontal and vertical split and to remove players.

### Changed

- In a world, controllers no longer go to player 1 by themselves.
- Controllers ask who is playing again in every world instead of keeping their player from the last one.

### Fixed

- Leaving and re-entering a world after playing split-screen crashed (LeviLamina kept a pointer to a player that had left).
- Reconnecting controllers could open more than four controller slots and crash.

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

[Unreleased]: https://github.com/SnakyWarrior/SplitScreenMod/compare/v1.1.1...HEAD
[1.1.1]: https://github.com/SnakyWarrior/SplitScreenMod/compare/v1.1.0...v1.1.1
[1.1.0]: https://github.com/SnakyWarrior/SplitScreenMod/compare/v1.0.1...v1.1.0
[1.0.1]: https://github.com/SnakyWarrior/SplitScreenMod/compare/v1.0.0...v1.0.1
[1.0.0]: https://github.com/SnakyWarrior/SplitScreenMod/releases/tag/v1.0.0
