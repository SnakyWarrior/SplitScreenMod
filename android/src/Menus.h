#pragma once

// The split screen menus and how a new controller joins: the Windows version's logic (src/mod/MyMod.cpp:
// openSettings, offerPicker, startJoin, watchChoice, checkControllers), with the same choices, the same texts (the
// SplitScreenUI resource pack draws them in the game's language) and the same rules. Only what the game itself does
// is different on Android; that part is the "game" functions below, which SplitScreen.cpp provides.
//
// On Android a player's sub client id is its controller number (player 1 = 0, on touch; players 2-4 = 1-3):
// SplitScreen.cpp keeps them equal by giving a joining controller the number of the first free player.

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace splitscreen {

namespace game {
// The players in the world: sub client id -> controller number. Player 1 is always there (sub 0).
std::map<int, int> players();
// Whether a controller is connected and plays as controller number `controller` (player 1's touch is not one).
bool hasController(int controller);
// What a player's own "Save & Leave" does.
void requestLeave(int sub);
// The controller of `from` now plays `to`, and `to`'s (if any) plays `from`.
void moveController(int from, int to);
// Adds a player for controller `controller`; the game gives it the first free sub client id. `joined` gets that id,
// or -1 when no player was added.
void join(int controller, std::function<void(int sub)> joined = {});
} // namespace game

namespace menus {
// The resource pack's texts are read from `pack` (its texts/ folder) in the game's language from `options`
// (the game's options.txt), English when that is not known.
void setTexts(std::filesystem::path const& pack, std::filesystem::path const& options);

// A controller with no player pressed Start (the game's "press Start to join").
void onJoinRequest(int controller);
// A player's controller pressed Select (View / Back): the Split screen menu (on Windows, the pause menu's button).
void onSettingsButton(int sub);
// A connected controller was given to player `sub`, who had lost theirs.
void onControllerConnected(int sub);
// A player left the world.
void onPlayerLeft(int sub);
// Once a second: players without a controller for 20 seconds leave.
void checkControllers();
} // namespace menus

} // namespace splitscreen
