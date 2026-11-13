#pragma once

// Groundwork for in-game menus (forms) on Android, as LeviLamina does them on Windows: a ModalFormRequestPacket
// (form id, JSON) sent to one local player through the server's LoopbackPacketSender::sendToClient(NetworkIdentifier,
// Packet, SubClientId), and the answer read in PacketHandlerDispatcherInstance<ModalFormResponsePacket>::handle.
// This version finds those classes by RTTI and logs what is needed to call them safely.

#include "Elf.h"
#include "InputProbe.h"

#include <functional>
#include <string>
#include <vector>

namespace splitscreen {

void startFormProbe(Module const& game, LogFn log);

// A menu like showMenu whose buttons carry pictures (texture paths; "" for none).
void showForm(int sub, std::string const& title, std::string const& content, std::vector<std::string> const& buttons,
              std::vector<std::string> const& pictures, std::function<void(int)> done, int timeoutSeconds = 0,
              int timeoutAnswer = -1);

// Forgets player `sub`'s menu (the player leaves); its answer, if one still comes, is ignored.
void cancelMenu(int sub);

// Whether player 1 is in a world (and has been for a few seconds: not while it loads).
bool inWorld();

// Whether player `sub` has a menu open (or about to open).
bool menuOpen(int sub);

// Shows player `sub` (sub client id) a menu of buttons; `done` gets the button index, -1 when it is closed. It runs
// on the server thread. With `timeoutSeconds`, the menu is closed that long after it shows and `done` gets
// `timeoutAnswer`. Titles "[ss:m]" (Split screen), "[ss:w]" (Who is playing?) and "[ss:c]" (Choose your player:
// black screen and a 10 second countdown) are drawn by the SplitScreenUI resource pack.
void showMenu(int sub, std::string const& title, std::string const& content, std::vector<std::string> const& buttons,
              std::function<void(int)> done, int timeoutSeconds = 0, int timeoutAnswer = -1);

} // namespace splitscreen
