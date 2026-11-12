#include "FormProbe.h"

#include "DeviceManagerProbe.h"
#include "VTrace.h"

#include <sys/uio.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <fmt/format.h>

namespace splitscreen {

namespace {

Module const* gGame = nullptr;
LogFn         gLog;

bool safeRead(uintptr_t address, void* out, size_t size) {
    iovec local{out, size}, remote{reinterpret_cast<void*>(address), size};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == ssize_t(size);
}

std::string offset(uintptr_t p) {
    return gGame && gGame->isCode(p) ? fmt::format("+0x{:X}", p - gGame->base) : fmt::format("0x{:X}", p);
}

struct Found {
    std::string name;
    uintptr_t*  vtable = nullptr;
    size_t      slots  = 0;
};

// Classes whose RTTI name contains `needle`, with their primary vtables.
std::vector<Found> find(char const* needle, bool log) {
    std::vector<Found> out;
    for (auto const& t : findTypeNames(*gGame, needle)) {
        uintptr_t ti = findTypeInfo(*gGame, t.address);
        if (!ti) continue;
        for (auto* vt : findVtables(*gGame, ti)) {
            Found f{t.name, vt, countSlots(*gGame, vt, 64)};
            if (log) {
                gLog(fmt::format("forms: class {} vtable +0x{:X}, {} slots", f.name,
                                 reinterpret_cast<uintptr_t>(vt) - gGame->base, f.slots));
                for (size_t i = 0; i < f.slots && i < 24; ++i)
                    gLog(fmt::format("forms:   [{}] {} {}", i, offset(vt[i]), describeFunction(*gGame, vt[i])));
            }
            out.push_back(f);
        }
    }
    return out;
}

// IPacketHandlerDispatcher::handle(NetworkIdentifier const& source, NetEventCallback& callback,
// std::shared_ptr<Packet>& packet) const: vtable slot 2 (after the two destructors).
using HandleFn = void (*)(void*, void*, void*, void*);
HandleFn          gAuthHandle = nullptr, gResponseHandle = nullptr;
std::mutex        gSeenMutex;
std::set<int>     gSeenSub;
std::atomic<bool> gTracingStarted{false};

uint8_t packetSubId(void* packetRef) {
    uintptr_t packet = 0;
    uint8_t   sub    = 0xFF;
    if (packetRef && safeRead(reinterpret_cast<uintptr_t>(packetRef), &packet, 8) && packet) safeRead(packet + 16, &sub, 1);
    return sub;
}

void authHandleDetour(void* self, void* source, void* callback, void* packetRef) {
    uint8_t sub = packetSubId(packetRef);
    bool    fresh;
    {
        std::lock_guard lock(gSeenMutex);
        fresh = gSeenSub.insert(sub).second;
    }
    if (fresh) {
        uint8_t id[160] = {};
        safeRead(reinterpret_cast<uintptr_t>(source), id, sizeof id);
        std::string bytes;
        for (int i = 0; i < 40; ++i) bytes += fmt::format("{:02x}", id[i]);
        gLog(fmt::format("forms: PlayerAuthInputPacket from sub client {}: source {} [{}...] type {} callback {} ({})", sub,
                         fmt::ptr(source), bytes, *reinterpret_cast<int32_t*>(id + 152),
                         fmt::ptr(callback), className(*gGame, reinterpret_cast<uintptr_t>(callback))));
        if (!gTracingStarted.exchange(true)) startTracing(30); // the packet sender's calls while in a world
    }
    gAuthHandle(self, source, callback, packetRef);
}

void responseHandleDetour(void* self, void* source, void* callback, void* packetRef) {
    uintptr_t packet = 0;
    uint32_t  formId = 0;
    if (safeRead(reinterpret_cast<uintptr_t>(packetRef), &packet, 8) && packet) safeRead(packet + 0x30, &formId, 4);
    gLog(fmt::format("forms: ModalFormResponsePacket from sub client {}, form id {}", packetSubId(packetRef), formId));
    gResponseHandle(self, source, callback, packetRef);
}

void hookHandle(Found const& f, void* detour, HandleFn* original, char const* what) {
    if (f.slots < 3) return gLog(fmt::format("forms: ERROR: {} has {} slots", what, f.slots));
    *original = reinterpret_cast<HandleFn>(f.vtable[2]);
    bool ok   = writeSlot(&f.vtable[2], reinterpret_cast<uintptr_t>(detour));
    gLog(fmt::format("forms: {} handle {} {}", what, offset(reinterpret_cast<uintptr_t>(*original)),
                     ok ? "watched" : "ERROR: not written"));
}

} // namespace

void startFormProbe(Module const& game, LogFn log) {
    static Module copy = game;
    gGame              = &copy;
    gLog               = std::move(log);

    // the packets themselves: getId should return 100 (request) and 101 (response)
    find("22ModalFormRequestPacket", true);
    find("23ModalFormResponsePacket", true);

    // the server's packet sender: traced (its slots, arguments and callers) while in a world
    auto senders = find("20LoopbackPacketSender", true);
    for (auto const& s : senders)
        if (s.name == "20LoopbackPacketSender" && s.slots)
            traceVtable(copy, s.vtable, s.slots, 400, "LoopbackPacketSender", gLog);

    // dispatchers: PlayerAuthInputPacket (every local player sends it) and ModalFormResponsePacket
    for (auto const& d : find("PacketHandlerDispatcherInstanceI21PlayerAuthInputPacket", true))
        if (!gAuthHandle) hookHandle(d, reinterpret_cast<void*>(&authHandleDetour), &gAuthHandle, "PlayerAuthInputPacket");
    for (auto const& d : find("PacketHandlerDispatcherInstanceI23ModalFormResponsePacket", true))
        if (!gResponseHandle)
            hookHandle(d, reinterpret_cast<void*>(&responseHandleDetour), &gResponseHandle, "ModalFormResponsePacket");
}

} // namespace splitscreen
