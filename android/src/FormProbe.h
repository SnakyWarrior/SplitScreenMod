#pragma once

// Groundwork for in-game menus (forms) on Android, as LeviLamina does them on Windows: a ModalFormRequestPacket
// (form id, JSON) sent to one local player through the server's LoopbackPacketSender::sendToClient(NetworkIdentifier,
// Packet, SubClientId), and the answer read in PacketHandlerDispatcherInstance<ModalFormResponsePacket>::handle.
// This version finds those classes by RTTI and logs what is needed to call them safely.

#include "Elf.h"
#include "InputProbe.h"

namespace splitscreen {

void startFormProbe(Module const& game, LogFn log);

} // namespace splitscreen
