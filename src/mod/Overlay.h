#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// A menu drawn in its own window over the centre of the game, not inside any player's part of the split screen.
// Driven by any XInput controller (D-pad/stick, A, B), the keyboard (arrows, Enter, Esc) or the mouse.
namespace overlay {

// 8x8 pixels (COLORREF, row by row), e.g. a player's face; empty = no picture.
using Icon = std::vector<uint32_t>;

// `done` runs on the game's client thread with the chosen index, or -1 when cancelled.
// `icons`, if given, has one entry per button.
void show(
    std::string              title,
    std::string              text,
    std::vector<std::string> buttons,
    std::function<void(int)> done,
    std::vector<Icon>        icons = {}
);

// A small "Split screen" button in the corner of the game (for the pause menu). Clicking it, or Y on a controller,
// runs `click` on the client thread. Hidden while a menu is open.
void showButton(std::function<void()> click);
void hideButton();

bool isOpen();

} // namespace overlay
