#pragma once

// Groundwork for in-game menus (forms) on Android, as LeviLamina does them on Windows: a ModalFormRequestPacket
// (form id, JSON) sent to one local player through the server's LoopbackPacketSender::sendToClient(NetworkIdentifier,
// Packet, SubClientId), and the answer read in PacketHandlerDispatcherInstance<ModalFormResponsePacket>::handle.
// This version finds those classes by RTTI and logs what is needed to call them safely.

#include "Elf.h"
#include "InputProbe.h"

namespace splitscreen {

void startFormProbe(Module const& game, LogFn log);

// Sends sub client `sub` (an added player) a test form 5 seconds from now.
void requestTestForm(int sub);

} // namespace splitscreen
