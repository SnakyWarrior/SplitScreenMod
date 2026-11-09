#pragma once

#include <functional>
#include <string>
#include <vector>

// A menu drawn in its own window over the centre of the game, not inside any player's part of the split screen.
// Driven by any XInput controller (D-pad/stick, A, B), the keyboard (arrows, Enter, Esc) or the mouse.
namespace overlay {

// `done` runs on the game's client thread with the chosen index, or -1 when cancelled.
void show(std::string title, std::string text, std::vector<std::string> buttons, std::function<void(int)> done);

bool isOpen();

} // namespace overlay
