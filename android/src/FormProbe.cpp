#include "FormProbe.h"

#include "DeviceManagerProbe.h"
#include "VTrace.h"

#include <sys/uio.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <new>
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

// --- sending a form ---------------------------------------------------------------------------------------------
// LoopbackPacketSender virtual 7: sendToClient(NetworkIdentifier const&, Packet const&, SubClientId) (0.5.0: it is the
// one called with the local players' NetworkIdentifier). Its first call gives the server's sender object.
using SendToClientFn = void (*)(void*, void const*, void const*, uint32_t);
SendToClientFn    gSendToClient = nullptr;
std::atomic<void*> gSender{nullptr};

void sendToClientDetour(void* self, void const* id, void const* packet, uint32_t sub) {
    if (!gSender.exchange(self)) gLog(fmt::format("forms: the server's packet sender is {}", self));
    gSendToClient(self, id, packet, sub);
}

// ModalFormRequestPacket on Android (checked: getId at slot 2 returns 100, getSerializationMode reads +0x50):
//   Packet: vptr, priority +8, reliability +12, sender sub client +16, handled +17, receive time +24,
//           handler dispatcher +32, compressibility +40; then form id +0x30, form JSON (std::string) +0x38,
//           serialization mode +0x50.
uintptr_t gRequestVtable    = 0; // address point of ModalFormRequestPacket's vtable
uintptr_t gRequestDispatcher = 0; // the PacketHandlerDispatcherInstance<ModalFormRequestPacket> object

struct RequestPacket {
    uintptr_t   vptr;
    int32_t     priority;     // MediumPriority
    int32_t     reliability;  // ReliableOrdered
    uint8_t     senderSubId;
    bool        handled;
    uint8_t     pad[6];
    int64_t     receiveTime;
    uintptr_t   handler;
    int32_t     compressible; // Compressible
    int32_t     pad2;
    uint32_t    formId;
    uint32_t    pad3;
    std::string json;
    int32_t     serializationMode;
    int32_t     pad4;
};
#ifdef _LIBCPP_VERSION // the game's std::string (libc++, 24 bytes)
static_assert(offsetof(RequestPacket, formId) == 0x30 && offsetof(RequestPacket, json) == 0x38
              && offsetof(RequestPacket, serializationMode) == 0x50);
#endif

// ClientboundCloseFormPacket: closes the open form; only the Packet base (0x30 bytes, room left for more).
uintptr_t gCloseVtable = 0, gCloseDispatcher = 0;
struct ClosePacket {
    uintptr_t vptr;
    int32_t   priority;
    int32_t   reliability;
    uint8_t   senderSubId;
    bool      handled;
    uint8_t   pad[6];
    int64_t   receiveTime;
    uintptr_t handler;
    int32_t   compressible;
    int32_t   pad2;
    uint8_t   room[0x40];
};

bool sendCloseForm(void const* id, uint32_t sub) {
    void* sender = gSender.load();
    if (!sender || !gSendToClient || !gCloseVtable || !gCloseDispatcher) return false;
    auto* packet = new ClosePacket{gCloseVtable, 2, 1, 0, false, {}, 0, gCloseDispatcher, 0, 0, {}};
    gLog(fmt::format("forms: closing the form of sub client {}", sub));
    gSendToClient(sender, id, packet, sub);
    return true;
}

// The packet is kept (never freed): the loopback may hand the object itself to the client.
bool sendForm(void const* id, uint32_t sub, uint32_t formId, std::string json) {
    void* sender = gSender.load();
    if (!sender || !gSendToClient || !gRequestVtable || !gRequestDispatcher) {
        gLog(fmt::format("forms: cannot send yet (sender {}, vtable {}, dispatcher {})", sender, gRequestVtable != 0,
                         gRequestDispatcher != 0));
        return false;
    }
    auto* packet = new RequestPacket{gRequestVtable, 2, 1, 0, false, {}, 0, gRequestDispatcher, 0, 0, formId, 0,
                                     std::move(json), 0, 0};
    gLog(fmt::format("forms: sending form {} to sub client {}", formId, sub));
    gSendToClient(sender, id, packet, sub);
    gLog(fmt::format("forms: form {} sent", formId));
    return true;
}

// Player 1 in a world: its PlayerAuthInputPacket keeps coming (one a tick), for 3 seconds at least.
std::atomic<int64_t> gFirstAuth{0}, gLastAuth{0};
int64_t              nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::atomic<int64_t> gLastFormAt[4] = {}; // when each player's last menu was sent or answered (ms)

// Menus: one pending form per player (sub client), sent from that player's next PlayerAuthInputPacket (the server
// thread). A busy player (another screen open) answers "UserBusy" at once: the form is sent again half a second
// later, for up to 20 seconds. The answer: a Json::Value (+0x38: 8-byte value, type +0x40, 1 = int) holding the button index.
struct Pending {
    uint32_t                              formId = 0;
    std::string                           json;
    std::function<void(int)>              done;
    std::chrono::steady_clock::time_point due;
    int                                   tries = 0;
    std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();
    std::chrono::seconds                  timeout{0}; // closed with `timeoutAnswer` this long after it shows
    int                                   timeoutAnswer = -1;
    std::chrono::steady_clock::time_point shown{};
};
std::map<int, Pending> gPending; // sub client -> its form
uint32_t               gNextFormId = 0x5C5C0001;

void maybeSendMenu(void const* source, int sub) {
    uint32_t                 formId = 0;
    std::string              json;
    std::function<void(int)> expired;
    int                      answer = -1;
    {
        std::lock_guard lock(gSeenMutex);
        auto it = gPending.find(sub);
        if (it == gPending.end()) return;
        auto  now = std::chrono::steady_clock::now();
        auto& p   = it->second;
        if (p.timeout.count() && p.tries > 0 && now > p.shown + p.timeout) {
            expired = std::move(p.done);
            answer  = p.timeoutAnswer;
            gPending.erase(it);
        } else {
            if (now < p.due) return;
            p.shown = now; // the countdown starts with the send that shows (a busy player answers the earlier ones)
            ++p.tries;
            p.due  = now + std::chrono::hours(1); // until it is answered
            formId = p.formId;
            json   = p.json;
        }
    }
    if (sub >= 0 && sub < 4) gLastFormAt[sub] = nowMs();
    if (formId) {
        sendForm(source, uint32_t(sub), formId, std::move(json));
        return;
    }
    sendCloseForm(source, uint32_t(sub));
    gLog(fmt::format("menus: player {} did not choose in time: {}", sub + 1, answer));
    if (expired) expired(answer);
}

// The answer to form `formId` from `sub`: button index, -1 when closed; busy sends it again.
void menuAnswered(int sub, uint32_t formId, bool busy, int button) {
    if (sub >= 0 && sub < 4) gLastFormAt[sub] = nowMs();
    std::function<void(int)> done;
    {
        std::lock_guard lock(gSeenMutex);
        auto it = gPending.find(sub);
        if (it == gPending.end() || it->second.formId != formId) return;
        if (busy && it->second.tries < 40) { // busy while the player loads or another screen is open
            it->second.due = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            return;
        }
        done = std::move(it->second.done);
        gPending.erase(it);
    }
    gLog(fmt::format("menus: player {} chose {}", sub + 1, button));
    if (done) done(busy ? -1 : button);
}

std::string jsonString(std::string const& text) {
    std::string out = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) < 0x20) {
            out += fmt::format("\\u{:04x}", int(c));
            continue;
        }
        out += c;
    }
    return out + "\"";
}

uint8_t packetSubId(void* packetRef) {
    uintptr_t packet = 0;
    uint8_t   sub    = 0xFF;
    if (packetRef && safeRead(reinterpret_cast<uintptr_t>(packetRef), &packet, 8) && packet) safeRead(packet + 16, &sub, 1);
    return sub;
}

void authHandleDetour(void* self, void* source, void* callback, void* packetRef) {
    uint8_t sub = packetSubId(packetRef);
    if (sub == 0) {
        int64_t now = nowMs();
        if (now - gLastAuth > 1000) gFirstAuth = now; // a new stretch (a world was left or is loading)
        gLastAuth = now;
    }
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
    maybeSendMenu(source, sub);
}

void responseHandleDetour(void* self, void* source, void* callback, void* packetRef) {
    uintptr_t packet = 0;
    uint32_t  formId = 0;
    if (safeRead(reinterpret_cast<uintptr_t>(packetRef), &packet, 8) && packet) safeRead(packet + 0x30, &formId, 4);
    // ModalFormResponsePacket: form id +0x30, optional<Json::Value> +0x38 (value 16 bytes, engaged +0x48),
    // optional<ModalFormCancelReason> +0x50 (engaged +0x51)
    uint8_t answer[0x28] = {};
    if (packet) safeRead(packet + 0x30, answer, sizeof answer);
    std::string raw;
    for (int i = 8; i < 0x28; ++i) raw += fmt::format("{:02x}", answer[i]);
    gLog(fmt::format("forms: ModalFormResponsePacket from sub client {}, form id 0x{:X}: json value [{:016X} {:016X}] "
                     "engaged {}, cancel reason {} engaged {} (raw +0x38: {})",
                     packetSubId(packetRef), formId, *reinterpret_cast<uint64_t*>(answer + 8),
                     *reinterpret_cast<uint64_t*>(answer + 16), answer[0x18], answer[0x20], answer[0x21], raw));
    if (formId >> 16 == 0x5C5C) {
        bool    busy   = answer[0x21] && answer[0x20] == 1;
        int64_t value  = *reinterpret_cast<int64_t*>(answer + 8);
        int     button = answer[0x18] && answer[0x10] == 1 ? int(value) : -1;
        menuAnswered(packetSubId(packetRef), formId, busy, button);
    }
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


int64_t msSinceMenu(int sub) { return sub >= 0 && sub < 4 ? nowMs() - gLastFormAt[sub] : INT64_MAX; }

bool inWorld() {
    int64_t now = nowMs();
    return now - gLastAuth < 1000 && now - gFirstAuth > 3000;
}

bool menuOpen(int sub) {
    std::lock_guard lock(gSeenMutex);
    auto it = gPending.find(sub);
    if (it == gPending.end()) return false;
    // a menu that was never answered (the player left, the world closed) stops counting after a minute
    if (it->second.created + std::chrono::minutes(1) < std::chrono::steady_clock::now()) {
        gPending.erase(it);
        return false;
    }
    return true;
}

void cancelMenu(int sub) {
    std::lock_guard lock(gSeenMutex);
    gPending.erase(sub);
}

void showMenu(int sub, std::string const& title, std::string const& content, std::vector<std::string> const& buttons,
              std::function<void(int)> done, int timeoutSeconds, int timeoutAnswer) {
    showForm(sub, title, content, buttons, {}, std::move(done), timeoutSeconds, timeoutAnswer);
}

void showForm(int sub, std::string const& title, std::string const& content, std::vector<std::string> const& buttons,
              std::vector<std::string> const& pictures, std::function<void(int)> done, int timeoutSeconds,
              int timeoutAnswer) {
    std::string json = "{\"type\":\"form\",\"title\":" + jsonString(title) + ",\"content\":" + jsonString(content)
                     + ",\"buttons\":[";
    for (size_t i = 0; i < buttons.size(); ++i) {
        json += (i ? "," : "") + std::string("{\"text\":") + jsonString(buttons[i]);
        if (i < pictures.size() && !pictures[i].empty())
            json += ",\"image\":{\"type\":\"path\",\"data\":" + jsonString(pictures[i]) + "}";
        json += "}";
    }
    json += "]}";
    std::lock_guard lock(gSeenMutex);
    Pending p{gNextFormId++, std::move(json), std::move(done), std::chrono::steady_clock::now(), 0};
    p.timeout       = std::chrono::seconds(timeoutSeconds);
    p.timeoutAnswer = timeoutAnswer;
    gPending[sub]   = std::move(p);
    if (gNextFormId > 0x5C5CFFFF) gNextFormId = 0x5C5C0001;
}

void startFormProbe(Module const& game, LogFn log) {
    static Module copy = game;
    gGame              = &copy;
    gLog               = std::move(log);

    // the packets themselves: getId returns 100 (request) and 101 (response)
    for (auto const& f : find("22ModalFormRequestPacket", false)) {
        if (f.name == "22ModalFormRequestPacket" && f.slots > 2 && constantReturn(f.vtable[2]) == 100u)
            gRequestVtable = reinterpret_cast<uintptr_t>(f.vtable);
        // the dispatcher object: a word in the game's data equal to its vtable
        if (f.name == "31PacketHandlerDispatcherInstanceI22ModalFormRequestPacketLb0EE") {
            int count = 0;
            for (auto const& seg : copy.segments) {
                if (!seg.read || !seg.write) continue;
                for (uintptr_t p = (seg.start + 7) & ~uintptr_t(7); p + 8 <= seg.end; p += 8)
                    if (*reinterpret_cast<uintptr_t const*>(p) == reinterpret_cast<uintptr_t>(f.vtable)) {
                        if (!gRequestDispatcher) gRequestDispatcher = p;
                        ++count;
                    }
            }
            gLog(fmt::format("forms: request dispatcher object {} ({} found)",
                             gRequestDispatcher ? offset(gRequestDispatcher) : "none", count));
        }
    }
    gLog(fmt::format("forms: ModalFormRequestPacket vtable {}", gRequestVtable ? offset(gRequestVtable) : "not found"));
    // ClientboundCloseFormPacket (closes a form when its time is up) and its dispatcher object
    for (auto const& f : find("26ClientboundCloseFormPacket", false)) {
        if (f.name == "26ClientboundCloseFormPacket" && f.slots > 2) gCloseVtable = reinterpret_cast<uintptr_t>(f.vtable);
        if (f.name == "31PacketHandlerDispatcherInstanceI26ClientboundCloseFormPacketLb0EE")
            for (auto const& seg : copy.segments) {
                if (!seg.read || !seg.write || gCloseDispatcher) continue;
                for (uintptr_t p = (seg.start + 7) & ~uintptr_t(7); p + 8 <= seg.end; p += 8)
                    if (*reinterpret_cast<uintptr_t const*>(p) == reinterpret_cast<uintptr_t>(f.vtable)) {
                        gCloseDispatcher = p;
                        break;
                    }
            }
    }
    gLog(fmt::format("forms: ClientboundCloseFormPacket vtable {} (id {}), dispatcher {}",
                     gCloseVtable ? offset(gCloseVtable) : "not found",
                     gCloseVtable ? int(constantReturn(reinterpret_cast<uintptr_t*>(gCloseVtable)[2]).value_or(0)) : -1,
                     gCloseDispatcher ? offset(gCloseDispatcher) : "not found"));

    // the server's packet sender: traced (its slots, arguments and callers) while in a world
    auto senders = find("20LoopbackPacketSender", false);
    for (auto const& s : senders)
        if (s.name == "20LoopbackPacketSender" && s.slots > 7) {
            gSendToClient = reinterpret_cast<SendToClientFn>(s.vtable[7]);
            writeSlot(&s.vtable[7], reinterpret_cast<uintptr_t>(&sendToClientDetour));
            traceVtable(copy, s.vtable, s.slots, 400, "LoopbackPacketSender", gLog);
        }

    // dispatchers: PlayerAuthInputPacket (every local player sends it) and ModalFormResponsePacket
    for (auto const& d : find("PacketHandlerDispatcherInstanceI21PlayerAuthInputPacket", false))
        if (!gAuthHandle) hookHandle(d, reinterpret_cast<void*>(&authHandleDetour), &gAuthHandle, "PlayerAuthInputPacket");
    for (auto const& d : find("PacketHandlerDispatcherInstanceI23ModalFormResponsePacket", false))
        if (!gResponseHandle)
            hookHandle(d, reinterpret_cast<void*>(&responseHandleDetour), &gResponseHandle, "ModalFormResponsePacket");
}

} // namespace splitscreen
