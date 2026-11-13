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

// Shows player `sub` (sub client id) a menu of buttons; `done` gets the button index, -1 when it is closed. It runs
// on the server thread.
void showMenu(int sub, std::string const& title, std::string const& content, std::vector<std::string> const& buttons,
              std::function<void(int)> done);

} // namespace splitscreen
