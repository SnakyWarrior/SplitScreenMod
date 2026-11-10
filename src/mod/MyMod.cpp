#include "mod/MyMod.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <Windows.h>
#include <intrin.h>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "ll/api/memory/Hook.h"
#include "ll/api/thread/ClientThreadExecutor.h"
#include "ll/api/memory/Memory.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/TargetedBedrock.h"
#include "ll/api/utils/StacktraceUtils.h"
#include "mc/client/game/ClientInstance.h"
#include "mc/client/game/ClientInstanceArguments.h"
#include "mc/client/game/IClientInstance.h"
#include "mc/client/options/IOptionRegistry.h"
#include "mc/deps/renderer/ViewportInfo.h"
#include "mc/common/Globals.h"
#include "mc/client/game/IMinecraftGame.h"
#include "mc/client/input/MinecraftInputHandler.h"
#include "mc/deps/application/gamecore/Platform_GameCore.h"
#include "mc/deps/entry/src_gamecore/input/GameCorePlayerInput.h"
#include "mc/gamecore/AppPlatform_GameCore.h"
#include "mc/gamecore/GameControllerHandler_GameCore.h"
#include "mc/deps/input/InputHandler.h"
#include "mc/locale/Localization.h"
#include "mc/deps/core/math/Color.h"
#include "mc/network/LoopbackPacketSender.h"
#include "mc/network/packet/TextPacket.h"
#include "mc/world/effect/MobEffectInstance.h"
#include "mc/client/gui/screens/controllers/PauseScreenController.h"
#include "mc/client/gui/screens/models/PauseScreenModel.h"
#include "mc/client/gui/ViewRequest.h"
#include "mc/client/gui/screens/interfaces/ISceneStack.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"
#include "ll/api/form/SimpleForm.h"
#include "ll/api/service/Bedrock.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/deps/application/AppPlatform.h"

namespace my_mod {

// LeviLamina exports no addresses for these PC overrides in this game build,
// so each one is read from the live object's vtable and hooked there.
// Patch the vtable entry, not the function body: on PC allowSplitScreen is a plain "return false" that the
// linker folded with ~1500 other functions, so hooking its code changed unrelated game behaviour.
struct Hooked {
    ll::memory::FuncPtr* slot;
    ll::memory::FuncPtr  original;
};
static std::vector<Hooked> gHooks;

// LeviLamina's own log file is not written in this setup, so keep our own next to the mod.
// One stream for all note() instantiations; a static inside the template would reopen (and truncate) per type.
static void writeLine(std::string const& line) {
    static std::mutex m;
    std::lock_guard   lock(m);
    auto&       self = MyMod::getInstance().getSelf();
    static auto out  = std::ofstream(self.getModDir() / "splitscreen.log", std::ios::trunc);
    self.getLogger().info("{}", line);
    out << line << std::endl;
}

template <class... Args>
static void note(fmt::format_string<Args...> f, Args&&... args) {
    writeLine(fmt::format(f, std::forward<Args>(args)...));
}

static uintptr_t gameBase() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

// Per-hook behaviour from splitscreen.cfg: "trace" keeps the game's answer and logs each new caller,
// "force" returns true everywhere, "force <rva>..." returns true only for those call sites.
struct HookCfg {
    bool                    autoSites = false; // pick the call sites by what the calling code does (see autoAllow)
    bool                    force = false;
    std::set<uintptr_t>     only;
    std::set<uintptr_t>     seen;
    ll::memory::FuncPtr     origin = nullptr;
};
static std::map<std::string, HookCfg> gCfg;

static uintptr_t gJoinFn = 0; // IMinecraftGame::isSplitscreenJoinEnabled, read from the game's vtable in enable()

// True if the code contains "test ..., 0x40000" (GameInputKindGamepad): the code that hands new gamepads to players.
static bool testsGamepadKind(uint8_t const* body, size_t n) {
    for (size_t p = 6; p + 4 <= n; ++p)
        if (std::memcmp(body + p, "\x00\x00\x04\x00", 4) == 0
            && (body[p - 2] == 0xF7 || body[p - 3] == 0xF7 || body[p - 6] == 0xF7))
            return true;
    return false;
}

// True if the code builds the std::function of SplitscreenJoinListener::registerInputHandlers ("press a button to
// join"). Found by RTTI name, which survives updates: the function loads a vtable whose _Target_type entry
// (slot 3, "lea rax,[type_info]; ret") returns a type whose name mentions SplitscreenJoinListener.
static bool buildsJoinListener(uint8_t const* body, size_t n) {
    auto mod   = gameBase();
    auto nt    = reinterpret_cast<IMAGE_NT_HEADERS const*>(mod + reinterpret_cast<IMAGE_DOS_HEADER const*>(mod)->e_lfanew);
    auto end   = mod + nt->OptionalHeader.SizeOfImage;
    auto inMod = [&](uintptr_t a, size_t len) { return a >= mod && a + len <= end; };
    for (size_t p = 0; p + 7 <= n; ++p) {
        // lea r64, [rip+disp32]
        if ((body[p] & 0xFB) != 0x48 || body[p + 1] != 0x8D || (body[p + 2] & 0xC7) != 0x05) continue;
        auto vt = reinterpret_cast<uintptr_t>(body + p + 7) + *reinterpret_cast<int32_t const*>(body + p + 3);
        if (!inMod(vt, 32)) continue;
        auto fn = reinterpret_cast<uintptr_t const*>(vt)[3];
        if (!inMod(fn, 8)) continue;
        auto c = reinterpret_cast<uint8_t const*>(fn);
        if (c[0] != 0x48 || c[1] != 0x8D || c[2] != 0x05 || c[7] != 0xC3) continue;
        auto ti = fn + 7 + *reinterpret_cast<int32_t const*>(c + 3);
        if (!inMod(ti + 16, 256)) continue;
        auto name = reinterpret_cast<char const*>(ti + 16);
        if (std::string_view(name, strnlen(name, 256)).find("@SplitscreenJoinListener@@") != std::string_view::npos)
            return true;
    }
    return false;
}

// Version-independent choice of which allowSplitScreen callers get "true": the splitscreen join check, the input
// setup that creates the "press a button to join" listener, and the GameInput code that would otherwise give every
// gamepad to player 1. Everything else keeps the PC answer,
// since forcing those callers broke the game.
static RUNTIME_FUNCTION* functionAt(uintptr_t addr, uintptr_t& begin) {
    DWORD64 base = 0;
    auto    fn   = RtlLookupFunctionEntry(addr, &base, nullptr);
    begin        = fn ? base + fn->BeginAddress : 0;
    return fn;
}

// The add-a-device-to-this-player function asks allowSplitScreen too, and "true" makes it refuse gamepads.
enum class AddPath { Direct, UserPlayer, FreeSlot };
// UserPlayer: its first use in the device callback, adding to the signed-in user's player (on PC every gamepad would
// land there). FreeSlot: the callback reusing a free player slot, e.g. for a reconnected controller.
static AddPath addPath(void* ret) {
    void* frames[24];
    auto  n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
    int   i = 0;
    while (i < n && frames[i] != ret) ++i;
    if (i + 1 >= n) return AddPath::Direct;
    auto      outer = reinterpret_cast<uintptr_t>(frames[i + 1]);
    static std::map<uintptr_t, AddPath> cache; // decide() holds the lock
    if (auto it = cache.find(outer); it != cache.end()) return it->second;
    uintptr_t inner = 0, caller = 0;
    auto      innerFn = functionAt(reinterpret_cast<uintptr_t>(ret), inner);
    auto      outerFn = functionAt(outer, caller);
    auto      path    = AddPath::Direct;
    if (innerFn && outerFn) {
        auto body = reinterpret_cast<uint8_t const*>(caller);
        auto size = outerFn->EndAddress - outerFn->BeginAddress;
        if (testsGamepadKind(body, size)) {
            uintptr_t firstCall = 0; // address after the first direct call to the inner function
            for (size_t p = 0; p + 5 <= size && !firstCall; ++p)
                if (body[p] == 0xE8 && caller + p + 5 + *reinterpret_cast<int32_t const*>(body + p + 1) == inner)
                    firstCall = caller + p + 5;
            if (firstCall) path = outer == firstCall ? AddPath::UserPlayer : AddPath::FreeSlot;
        }
    }
    return cache[outer] = path;
}

static bool autoAllow(void* ret) {
    enum Kind { Unknown, No, Yes, Gamepad };
    static std::map<void*, Kind> kinds; // callers run often; decide() holds the lock
    auto                         kind = kinds.contains(ret) ? kinds[ret] : Unknown;
    if (kind == Unknown) {
        uintptr_t begin = 0;
        auto      fn    = functionAt(reinterpret_cast<uintptr_t>(ret), begin);
        if (!fn) return false;
        auto body = reinterpret_cast<uint8_t const*>(begin);
        auto size = fn->EndAddress - fn->BeginAddress;
        kind      = begin == gJoinFn || buildsJoinListener(body, size) ? Yes
                  : testsGamepadKind(body, size)                      ? Gamepad
                                                                      : No;
        if (kind == No && !gJoinFn) return false; // may be the join function, which is found in enable()
        kinds[ret] = kind;
    }
    if (kind != Gamepad) return kind == Yes;
    bool yes = true;
    auto path = addPath(ret);
    if (path == AddPath::FreeSlot) yes = false; // let the controller take the slot
    if (path == AddPath::UserPlayer) {
        // The first gamepad belongs to player 1; every later one must get a player of its own.
        static bool firstGiven = false;
        yes = std::exchange(firstGiven, true);
    }
    return yes;
}

static bool decide(char const* name, void* ret, bool original) {
    static std::mutex m;
    std::lock_guard   lock(m);
    auto& c   = gCfg[name];
    auto  rva = reinterpret_cast<uintptr_t>(ret) - gameBase();
    bool  res = c.autoSites ? autoAllow(ret) || original
              : c.force && (c.only.empty() || c.only.contains(rva)) ? true
                                                                     : original;
    if (c.seen.insert(rva).second) note("{} called from {:X}: game said {}, returning {}", name, rva, original, res);
    return res;
}

static bool allowSplitScreenDetour(void* self) {
    auto orig = reinterpret_cast<bool (*)(void*)>(gCfg["allowSplitScreen"].origin)(self);
    return decide("allowSplitScreen", _ReturnAddress(), orig);
}

static bool joinEnabledDetour(void* self) {
    auto orig = reinterpret_cast<bool (*)(void*)>(gCfg["isSplitscreenJoinEnabled"].origin)(self);
    return decide("isSplitscreenJoinEnabled", _ReturnAddress(), orig);
}

static bool canAddSubClientDetour(void* self, int controllerId) {
    auto orig = reinterpret_cast<bool (*)(void*, int)>(gCfg["canAddSplitscreenSubClient"].origin)(self, controllerId);
    note("canAddSplitscreenSubClient(controller {}) from {:X}: game said {}", controllerId,
         reinterpret_cast<uintptr_t>(_ReturnAddress()) - gameBase(), orig);
    return decide("canAddSplitscreenSubClient", _ReturnAddress(), orig);
}

// LeviLamina's getVtableIndex rejects 16-byte member pointers (classes with several bases, like AppPlatform)
// and knows one thunk form, so decode clang's vcall thunk here: mov rax,[rcx] then [rax+disp8/disp32].
static std::optional<uint> thunkSlot(uint8_t const* code) {
    for (int i = 0; i < 4 && code[0] == 0xE9; ++i) code += 5 + *reinterpret_cast<int32_t const*>(code + 1);
    if (code[0] != 0x48 || code[1] != 0x8B || code[2] != 0x01) return std::nullopt;
    auto op = code + 3;
    if (op[0] == 0x48) ++op; // mov rax,[rax+disp] instead of jmp [rax+disp]
    int32_t disp;
    if (op[1] == 0x60 || op[1] == 0x40) disp = static_cast<int8_t>(op[2]);
    else if (op[1] == 0xA0 || op[1] == 0x80) disp = *reinterpret_cast<int32_t const*>(op + 2);
    else if (op[1] == 0x20 || op[1] == 0x00) disp = 0;
    else return std::nullopt;
    if (disp < 0 || disp % 8) return std::nullopt;
    return static_cast<uint>(disp / 8);
}

template <class F>
static ll::memory::FuncPtr* findSlot(void* object, F method, char const* name) {
    struct Raw {
        uint8_t const* code;
        int32_t        adjust; // only present in 16-byte member pointers
    } raw{};
    std::memcpy(&raw, &method, sizeof(method));
    auto index = thunkSlot(raw.code);
    if (!index) {
        note("ERROR: could not find the vtable slot for {} (thunk bytes {:02X})", name, fmt::join(raw.code, raw.code + 16, " "));
        return nullptr;
    }
    auto self = static_cast<char*>(object) + (sizeof(method) > 8 ? raw.adjust : 0);
    return *reinterpret_cast<ll::memory::FuncPtr**>(self) + *index;
}

template <class F>
static ll::memory::FuncPtr* hookSlot(void* object, F method, ll::memory::FuncPtr detour, char const* name, void* originOut = nullptr) {
    auto slot = findSlot(object, method, name);
    if (!slot) return nullptr;
    if (*slot == detour) return slot;
    Hooked h{slot, *slot};
    gCfg[name].origin = h.original; // detours read this, so set it before the slot goes live
    if (originOut) *static_cast<std::atomic<ll::memory::FuncPtr>*>(originOut) = h.original;
    ll::memory::modify(slot, sizeof(*slot), [&] { *slot = detour; });
    gHooks.push_back(h);
    note("Hooked {} (original rva {:X})", name, reinterpret_cast<uintptr_t>(h.original) - gameBase());
    return slot;
}

// splitscreen.cfg lets tests switch hooks and resolve crash addresses without rebuilding:
//   hook <name> auto           allowSplitScreen only: the default, call sites chosen by autoAllow
//   hook <name> trace          log every call site, keep the game's answer
//   hook <name> force [rva..]  return true (only at those call sites, if given)
//   lookup <hex rva>   log the game function at that offset from Minecraft.Windows.exe
static void readConfig(std::set<std::string>& hooks) {
    std::ifstream in(MyMod::getInstance().getSelf().getModDir() / "splitscreen.cfg");
    if (!in) {
        hooks                              = {"allowSplitScreen"};
        gCfg["allowSplitScreen"].autoSites = true;
        return;
    }
    for (std::string line; std::getline(in, line);) {
        std::istringstream words(line);
        std::string        cmd, arg;
        words >> cmd >> arg;
        if (cmd == "hook") {
            hooks.insert(arg);
            std::string mode, rva;
            words >> mode;
            gCfg[arg].force     = mode == "force";
            gCfg[arg].autoSites = mode == "auto";
            while (words >> rva) gCfg[arg].only.insert(std::stoull(rva, nullptr, 16));
        }
        if (cmd == "lookup" && !arg.empty()) {
            auto rva  = std::stoull(arg, nullptr, 16);
            auto addr = reinterpret_cast<void*>(gameBase() + rva);
            note("lookup {:X}: {}", rva, ll::utils::stacktrace_utils::toString(ll::StacktraceEntry(addr)));
        }
    }
}

// Test trigger, so a second player can be added without a second physical controller or cheats:
// write "join <controller>" or "info" lines into trigger.txt next to the mod; it is run on the client thread and deleted.
static void dumpPlayers(IMinecraftGame& game);
static void offerPicker(SubClientId owner, bool fresh);
static std::set<int> gNoMenuFor; // sub ids that just joined through the menu itself (client thread only)
static thread_local bool tOurJoin = false; // joins the mod makes itself go through unchanged
static GameControllerHandler_GameCore* controllerHandler();

static void runTrigger(std::string const& cmd, int arg) {
    auto client = ll::service::getClientInstance();
    if (!client) return note("trigger {}: no client instance", cmd);
    auto& game = client->getMinecraftGame();
    if (cmd == "join") {
        note("adding player on controller {}", arg);
        game.addSubClientInstanceAndAssociateToUser(arg); // goes through the menu like the game's join prompt
    }
    if (cmd == "pads") dumpPlayers(game);
    if (cmd == "vp") // each player's viewport as the renderer and HUD see it
        for (auto& [sub, other] : game.getClientInstanceMap())
            if (other) {
                auto& info = other->getViewportInfo();
                auto  n    = other->getNormalizedViewportSize();
                note("vp: player {} size {} {} offset {} {} normalized {} {}", int(sub) + 1, info.size->x, info.size->y,
                     info.offset->x, info.offset->y, n.x, n.y);
            }
    if (cmd == "dir") client->getOptions().setSplitscreenDirection(arg); // 0 horizontal, 1 vertical
    // prototype: "form N" shows a test form to the Nth server-side player; "swap A B" swaps the gamepads of two
    // controller ids in the game's table.
    if (cmd == "who")
        if (auto level = ll::service::getLevel())
            level->forEachPlayer([](Player& player) {
                note("who: '{}' sub {} uuid {}", player.getRealName(), int(player.getClientSubId()), player.getUuid().asString());
                return true;
            });
    if (cmd == "form") {
        auto level = ll::service::getLevel();
        if (!level) return note("form: no level");
        int index = 0;
        level->forEachPlayer([&](Player& player) {
            note("form: player {} = {}", ++index, player.getRealName());
            if (index == arg)
                ll::form::SimpleForm("Bedrock Splitscreen", "Test form")
                    .appendButton("First")
                    .appendButton("Second")
                    .sendTo(player, [](Player& p, int choice, ll::form::FormCancelReason) {
                        note("form: {} chose {}", p.getRealName(), choice);
                    });
            return true;
        });
    }
    if (cmd == "swap") {
        auto handler = controllerHandler();
        if (!handler) return note("swap: no table");
        std::lock_guard lock(*handler->mPlayerInputsLock);
        auto& inputs = *handler->mPlayerInputs;
        int   other  = arg % 10;
        int   first  = arg / 10;
        if (!inputs.contains(first) || !inputs.contains(other)) return note("swap: no controller {} or {}", first, other);
        std::swap(inputs.at(first).mGamepadSupport, inputs.at(other).mGamepadSupport);
        note("swap: gamepads of controllers {} and {} swapped", first, other);
    }
    // test commands: "leave N" (the player's own Save & Leave), "remove N" (queueSubclientRemoval)
    if (cmd == "leave" || cmd == "remove")
        for (auto& [sub, other] : game.getClientInstanceMap())
            if (other && int(sub) + 1 == arg) {
                note("{} player {}", cmd, arg);
                if (cmd == "leave") other->requestLeaveGameAsync();
                else game.queueSubclientRemoval(sub);
            }
}

// A second player whose controller stays disconnected is removed, so their half of the screen does not stay frozen.
// The game reports controller (dis)connects through each client's InputHandler; we chain onto that callback.
static constexpr auto                                kRemoveAfter = std::chrono::seconds(20);
static std::map<int, std::chrono::steady_clock::time_point> gMissingSince; // controller id -> when it disconnected
static std::set<void*>                               gWatched;            // InputHandlers already chained

static void onControllerConnection(bool connected, int id) {
    note("controller {} {}", id, connected ? "connected" : "disconnected");
    if (connected) gMissingSince.erase(id);
    else gMissingSince.try_emplace(id, std::chrono::steady_clock::now());
}

static void watchConnections(IClientInstance& client) {
    auto input = client.getMinecraftInput();
    if (!input || !input->mInputHandler.get()) return;
    auto& handler = *input->mInputHandler.get();
    if (!gWatched.insert(&handler).second) return;
    auto& callback = handler.mControllerConnectionStateChangeHandler.get();
    callback       = [old = std::move(callback)](IClientInstance& c, bool connected, int id) {
        if (old) old(c, connected, id);
        onControllerConnection(connected, id);
    };
}

static std::mutex    gPlayerIdsLock;
static std::set<int> gPlayerIds; // controller ids that have a player, refreshed once a second on the client thread

static std::atomic_bool gInWorld = false; // refreshed on the client thread, read on GameInput's thread

// Controllers that asked to join while no world was loaded (e.g. the game bringing players back when a world opens):
// they are asked who they are once the world is up, so no controller keeps its player from the last world.
static std::set<int> gAskLater; // client thread only
// The mod's menus that are open, per player (see showMenu). A menu that got no answer in a minute is taken as
// gone (e.g. it was sent while that player was still loading). Client thread only.
static std::map<int, std::chrono::steady_clock::time_point> gMenus;
static bool menuOpen(int sub) {
    auto it = gMenus.find(sub);
    return it != gMenus.end() && std::chrono::steady_clock::now() - it->second < std::chrono::minutes(1);
}
static void          startJoin(IMinecraftGame& game, int slot);
static void          freeFirstPad(IMinecraftGame& game, void* pad);
static Player*       serverPlayer(SubClientId sub);
static int           gFreshSub = -1; // saved player that just joined and still has to be asked about (client thread)
static std::chrono::steady_clock::time_point gFreshAt;
static void          openSettings(int who = -1);
static int           pausedPlayer();

static std::atomic<int> gSecondsLeft = -1; // shown in the "controller lost" prompt; -1 when nobody is leaving

static std::vector<void*> gamepadsOf(GameCorePlayerInput& input);

static void checkControllers(IMinecraftGame& game) {
    auto now  = std::chrono::steady_clock::now();
    int  left = -1;
    // A slot that has a controller again is not missing one, whatever older events said (a new player may have
    // joined on a slot whose earlier controller was unplugged long ago).
    if (auto handler = controllerHandler()) {
        std::lock_guard lock(*handler->mPlayerInputsLock);
        for (auto& [id, input] : *handler->mPlayerInputs)
            if (!gamepadsOf(input).empty()) gMissingSince.erase(id);
    }
    {
        std::lock_guard lock(gPlayerIdsLock);
        gPlayerIds.clear();
        for (auto& [sub, client] : game.getClientInstanceMap())
            if (client) gPlayerIds.insert(client->getControllerId());
    }
    for (auto& [sub, client] : game.getClientInstanceMap()) {
        if (!client) continue;
        watchConnections(*client);
        if (sub == SubClientId::PrimaryClient) continue;
        auto lost = gMissingSince.find(client->getControllerId());
        if (lost != gMissingSince.end()) {
            auto secs = int(std::chrono::duration_cast<std::chrono::seconds>(kRemoveAfter - (now - lost->second)).count());
            if (secs >= 0 && (left < 0 || secs < left)) left = secs;
        }
        if (lost != gMissingSince.end() && now - lost->second > kRemoveAfter) {
            note("player {}: no controller for {} s, removed", int(sub) + 1, kRemoveAfter.count());
            gMissingSince.erase(lost);
            client->requestLeaveGameAsync(); // what the player's own "Save & Leave" does
        }
    }
    gSecondsLeft = left;

}

// While a new controller's player is still being chosen, the stand-in player that holds its part of the screen is
// hidden: no "joined"/"left"/"edited the character appearance" chat lines, invisible, and it cannot be hurt.
static std::atomic<std::chrono::steady_clock::rep> gQuietUntil = 0;
static void quietFor(std::chrono::seconds s) { gQuietUntil = (std::chrono::steady_clock::now() + s).time_since_epoch().count(); }

static bool isQuietJoinMessage(Packet const& packet) {
    if (std::chrono::steady_clock::now().time_since_epoch().count() > gQuietUntil || packet.getId() != MinecraftPacketIds::Text)
        return false;
    auto& body = *static_cast<TextPacket const&>(packet).mBody;
    auto  msg  = std::get_if<TextPacketPayload::MessageAndParams>(&body);
    // joined, left, and "edited the character appearance" (skin) lines
    return msg && msg->mMessage->find("multiplayer.player.") != std::string::npos;
}

LL_AUTO_TYPE_INSTANCE_HOOK(QuietBroadcastHook, ll::memory::HookPriority::Normal, LoopbackPacketSender,
                           &LoopbackPacketSender::$sendBroadcast, void, Packet const& packet) {
    if (!isQuietJoinMessage(packet)) origin(packet);
}

LL_AUTO_TYPE_INSTANCE_HOOK(QuietSendHook, ll::memory::HookPriority::Normal, LoopbackPacketSender,
                           &LoopbackPacketSender::$sendToClients, void,
                           std::vector<NetworkIdentifierWithSubId> const& ids, Packet const& packet) {
    if (!isQuietJoinMessage(packet)) origin(ids, packet);
}

static void hideStandIn(Player& player, bool hide) {
    constexpr uint kResistance   = 11; // level 5 resistance: no damage
    constexpr uint kInvisibility = 14; // the effect, so the other players' games hide it too
    if (hide) {
        MobEffectInstance resistance(kResistance, EffectDuration{20 * 60 * 10});
        resistance.mAmplifier = 4;
        player.addEffect(resistance);
        MobEffectInstance invisible(kInvisibility, EffectDuration{20 * 60 * 10});
        invisible.mEffectVisible = false;
        player.addEffect(invisible);
    } else {
        player.removeEffect(kResistance);
        player.removeEffect(kInvisibility);
    }
}

// The pause menu's "Split screen" button (added by the mod's resource pack) sends "button.splitscreen_menu".
LL_AUTO_TYPE_INSTANCE_HOOK(
    PauseButtonHook,
    ll::memory::HookPriority::Normal,
    PauseScreenController,
    &PauseScreenController::$ctor,
    void*,
    std::shared_ptr<PauseScreenModel> model,
    bool                              overlaySocialDrawerOnce
) {
    auto result = origin(std::move(model), overlaySocialDrawerOnce);
    note("pause menu opened: Split screen button ready");
    registerButtonPressedHandler(_getNameId("button.splitscreen_menu"), [](UIPropertyBag*) {
        note("pause menu: Split screen pressed");
        // close the pause menu: while it is open the local world is paused and its menus (forms) do not arrive
        ll::thread::ClientThreadExecutor::getDefault().execute([who = pausedPlayer()] {
            if (auto client = ll::service::getClientInstance()) {
                auto clients = client->getMinecraftGame().getClientInstanceMap();
                if (auto it = clients.find(SubClientId(who)); it != clients.end() && it->second) {
                    // player 1's screens are on the window's main stack, the other players' on their own
                    if (who == 0) it->second->getMainSceneStack()->schedulePopScreen(1);
                    else it->second->getClientSceneStack()->schedulePopScreen(1);
                }
            }
            // after the pause menu is gone, so the pop cannot take the new menu with it and the world runs again
            ll::thread::ClientThreadExecutor::getDefault().executeAfter(
                [who] {
                    if (auto c = ll::service::getClientInstance()) note("pause menu closed: top screen now {}", c->getTopScreenName());
                    openSettings(who);
                },
                std::chrono::milliseconds(400)
            );
        });
        return ui::ViewRequest::None;
    });
    return result;
}

// The game's "controller lost" prompt reads this text; add how long until that player is removed.
LL_AUTO_TYPE_INSTANCE_HOOK(
    LostControllerTextHook,
    ll::memory::HookPriority::Normal,
    Localization,
    &Localization::get,
    bool,
    std::string const&              id,
    std::string&                    out,
    std::vector<std::string> const& params
) {
    bool found = origin(id, out, params);
    if (int left = gSecondsLeft; found && left >= 0 && id == "gamepad_disconnect.reconnectController")
        out += fmt::format("\n\nThe player leaves in {} s.", left);
    return found;
}

// The game's own table of which input devices belong to which player (controller id).
static GameControllerHandler_GameCore* controllerHandler() {
    auto platform = ll::service::getAppPlatform();
    if (!platform) return nullptr;
    auto& gameCore = static_cast<AppPlatform_GameCore&>(*platform);
    auto  core     = gameCore.mPlatformGameCore.get();
    return core ? core->mGameControllerHandler_Shim.get() : nullptr;
}

// shortcut: GameCoreGamepadSupport has no layout in LeviLamina; a vector of 0x30-byte entries starting with the device
// pointer is read from the 1.26.60 code. Only used for the diagnostic dump.
static std::vector<void*> gamepadsOf(GameCorePlayerInput& input) {
    std::vector<void*> out;
    auto support = reinterpret_cast<uint8_t* const*>(input.mGamepadSupport.get());
    if (!support) return out;
    for (auto e = support[0]; e && e < support[1]; e += 0x30) out.push_back(*reinterpret_cast<void**>(e));
    return out;
}

// "vid:pid" of a GameInput device (IGameInputDevice::GetDeviceInfo, vtable slot 3; the info starts with vendor and
// product id).
static std::string deviceName(void* device) {
    using GetInfo     = long (*)(void*, uint16_t const**);
    uint16_t const* info = nullptr;
    auto            fn   = reinterpret_cast<GetInfo>((*reinterpret_cast<void***>(device))[3]);
    if (fn(device, &info) < 0 || !info) return fmt::format("{}", device);
    return fmt::format("{:04X}:{:04X}", info[0], info[1]);
}

static void dumpPlayers(IMinecraftGame& game) {
    if (auto handler = controllerHandler()) {
        std::unique_lock lock(*handler->mPlayerInputsLock, std::try_to_lock);
        if (!lock) return note("pads: table busy, try again");
        for (auto& [id, input] : *handler->mPlayerInputs)
            note("pads: controller {}: state {}, gamepads {}, keyboard {}, mouse {}", id,
                 int(input.mConnectionState), [&] {
                     std::vector<std::string> names;
                     for (auto d : gamepadsOf(input)) names.push_back(deviceName(d));
                     return fmt::format("{}", fmt::join(names, " "));
                 }(), bool(input.mKeyboardSupport),
                 bool(input.mMouseSupport));
    } else note("pads: controller table not found");
    for (auto& [sub, client] : game.getClientInstanceMap())
        if (client) note("pads: player {} uses controller {}", int(sub) + 1, client->getControllerId());
}

static void pollTrigger() {
    static int tick   = 0;
    auto       client = ll::service::getClientInstance();
    gInWorld          = client && ll::service::getLevel();
    auto       window = GetForegroundWindow();
    DWORD      pid    = 0;
    GetWindowThreadProcessId(window, &pid);
    static bool f7WasDown = false; // F7 opens the layout menu
    bool        f7        = (GetAsyncKeyState(VK_F7) & 0x8000) && pid == GetCurrentProcessId();
    if (f7 && !f7WasDown && client && !menuOpen(pausedPlayer())) openSettings();
    f7WasDown = f7;
    static int worldTicks = 0; // how long the world has been up, in polls
    worldTicks            = gInWorld ? worldTicks + 1 : 0;
    // A menu sent while that player is still loading is lost: wait until they see their HUD.
    auto seesHud = [&](int sub) {
        if (!client) return false;
        auto clients = client->getMinecraftGame().getClientInstanceMap();
        auto it      = clients.find(SubClientId(sub));
        return it != clients.end() && it->second && it->second->getTopScreenName().find("hud_screen") != std::string::npos;
    };
    static bool askedFirst = false; // once per world
    if (!gInWorld) askedFirst = false;
    if (!askedFirst && worldTicks >= 60 && seesHud(0)) { // a controller already on player 1 asks who it is for
        askedFirst = true;
        // (a menu on player 1 would hide every other player's part of the screen while it is open)
        void* pad = nullptr;
        if (auto handler = controllerHandler()) {
            std::lock_guard lock(*handler->mPlayerInputsLock);
            auto&           inputs = *handler->mPlayerInputs;
            if (inputs.contains(0))
                if (auto pads = gamepadsOf(inputs.at(0)); !pads.empty()) pad = pads.front();
        }
        if (pad) freeFirstPad(client->getMinecraftGame(), pad);
    }
    if (gFreshSub >= 0 && client) {
        static int hidden = -1; // the stand-in is hidden as soon as it is in the world
        auto standIn = serverPlayer(SubClientId(gFreshSub));
        if (standIn && hidden != gFreshSub) {
            hideStandIn(*standIn, true);
            hidden = gFreshSub;
        }
        if (standIn && seesHud(gFreshSub) && !menuOpen(gFreshSub)) {
            offerPicker(SubClientId(gFreshSub), true);
            gFreshSub = -1;
            hidden    = -1;
        } else if (std::chrono::steady_clock::now() - gFreshAt > std::chrono::seconds(60)) gFreshSub = -1, hidden = -1;
    }
    if (worldTicks > 60 && !gAskLater.empty() && gFreshSub < 0) { // 3 s in: the players have spawned
        int slot = *gAskLater.begin();
        gAskLater.erase(gAskLater.begin());
        startJoin(client->getMinecraftGame(), slot);
    }
    // With several players the game keeps running while someone is paused; stop it when every player is paused.
    static bool allPaused = false;
    if (client && gInWorld) {
        auto clients = client->getMinecraftGame().getClientInstanceMap();
        bool all     = clients.size() > 1;
        for (auto& [sub, other] : clients)
            if (!other || other->getTopScreenName().find("pause") == std::string::npos) all = false;
        if (all != allPaused) {
            if (auto level = ll::service::getLevel()) level->setSimPaused(all);
            note("all players paused: {}", all);
            allPaused = all;
        }
    } else allPaused = false;
    static std::string lastScreen; // logged so the pause check can be fixed if the game renames its screens
    if (auto top = client ? client->getTopScreenName() : std::string(); top != lastScreen) note("screen: {}", lastScreen = top);
    static bool f9WasDown = false; // F9 writes the controller table to the log (edge-detected)
    bool        f9        = (GetAsyncKeyState(VK_F9) & 0x8000) && pid == GetCurrentProcessId();
    if (f9 && !f9WasDown && client) dumpPlayers(client->getMinecraftGame());
    f9WasDown = f9;
    if (++tick % 20 == 0 && client) checkControllers(client->getMinecraftGame()); // once a second
    auto path = MyMod::getInstance().getSelf().getModDir() / "trigger.txt";
    if (std::filesystem::exists(path)) {
        std::vector<std::pair<std::string, int>> cmds;
        {
            std::ifstream in(path);
            for (std::string line; std::getline(in, line);) {
                std::istringstream words(line);
                std::string        cmd;
                int                arg = 0;
                if (words >> cmd) {
                    words >> arg;
                    cmds.emplace_back(cmd, arg);
                }
            }
        }
        std::error_code ec;
        std::filesystem::remove(path, ec);
        for (auto& [cmd, arg] : cmds) runTrigger(cmd, arg);
    }
    ll::thread::ClientThreadExecutor::getDefault().executeAfter(pollTrigger, std::chrono::milliseconds(50));
}

MyMod& MyMod::getInstance() {
    static MyMod instance;
    return instance;
}

static std::set<std::string> gWanted;
static std::atomic_bool      gPlatformHooked = false;
static auto fn(auto f) { return reinterpret_cast<ll::memory::FuncPtr>(f); }

// Gamepads are sorted into players when they connect, which happens before enable(), so hook as early as possible.
// While AppPlatform is being constructed its vtable pointer still moves from base to derived class, so re-hook
// whenever the slot no longer holds our detour, and put the base class slot back.
static void hookPlatform() {
    static std::mutex                 m;
    static ll::memory::FuncPtr*       hooked = nullptr;
    std::lock_guard                   lock(m);
    auto                              platform = ll::service::getAppPlatform();
    if (!platform || !gWanted.contains("allowSplitScreen")) return;
    auto slot = hookSlot(platform.as_ptr(), &AppPlatform::allowSplitScreen, fn(&allowSplitScreenDetour), "allowSplitScreen");
    if (slot && slot != hooked && hooked) {
        auto old = std::find_if(gHooks.begin(), gHooks.end(), [](auto& h) { return h.slot == hooked; });
        if (old != gHooks.end()) {
            ll::memory::modify(old->slot, sizeof(*old->slot), [&] { *old->slot = old->original; });
            gHooks.erase(old);
        }
    }
    if (slot) hooked = slot;
    gPlatformHooked = slot != nullptr;
}

// The game's GameInput device-status callback hands new controllers to players. Found without addresses: it is the
// function that tests GameInputKindGamepad and directly calls another function that does too (adding the device).
static uintptr_t findDeviceCallback() {
    auto mod = gameBase();
    auto nt  = reinterpret_cast<IMAGE_NT_HEADERS const*>(mod + reinterpret_cast<IMAGE_DOS_HEADER const*>(mod)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    std::map<uintptr_t, size_t> gamepadFns; // begin -> size
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        auto text = reinterpret_cast<uint8_t const*>(mod + sec->VirtualAddress);
        for (size_t p = 6; p + 4 <= sec->Misc.VirtualSize; ++p) {
            if (text[p + 2] != 0x04 || text[p] != 0 || text[p + 1] != 0 || text[p + 3] != 0) continue;
            if (text[p - 2] != 0xF7 && text[p - 3] != 0xF7 && text[p - 6] != 0xF7) continue;
            uintptr_t begin = 0;
            if (auto fn = functionAt(reinterpret_cast<uintptr_t>(text + p), begin))
                gamepadFns[begin] = fn->EndAddress - fn->BeginAddress;
        }
    }
    for (auto& [begin, size] : gamepadFns) {
        auto body = reinterpret_cast<uint8_t const*>(begin);
        for (size_t p = 0; p + 5 <= size; ++p)
            if (body[p] == 0xE8) {
                auto target = begin + p + 5 + *reinterpret_cast<int32_t const*>(body + p + 1);
                if (target != begin && gamepadFns.contains(target)) return begin;
            }
    }
    return 0;
}

// On PC every controller is linked to the one signed-in Xbox user, and the game sends a device of a known user to
// that user's player (player 1), dropping it if player 1 already has a gamepad. Devices without a user take the
// console route instead: a free player slot, else a new player. So while the game sorts a gamepad, its user lookup
// (XUserFindForDevice, called only from this callback) reports no user.
static thread_local bool tHideUser = false;

using FindUser = long (*)(void const*, void**);
static FindUser gFindUser = nullptr;

static long findUserDetour(void const* deviceId, void** user) {
    if (!tHideUser) return gFindUser(deviceId, user);
    *user = nullptr;
    return static_cast<long>(0x8007048F); // HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED)
}

// The lookup is the call right after "add rcx, 3Ah" (the device id inside GameInputDeviceInfo).
static uintptr_t findUserLookup(uintptr_t callback) {
    uintptr_t begin = 0;
    auto      fn    = functionAt(callback, begin);
    if (!fn) return 0;
    auto body = reinterpret_cast<uint8_t const*>(begin);
    auto size = fn->EndAddress - fn->BeginAddress;
    for (size_t p = 0; p + 4 <= size; ++p) {
        if (std::memcmp(body + p, "\x48\x83\xC1\x3A", 4) != 0) continue;
        for (size_t q = p + 4; q < p + 24 && q + 5 <= size; ++q)
            if (body[q] == 0xE8) return begin + q + 5 + *reinterpret_cast<int32_t const*>(body + q + 1);
    }
    return 0;
}

// GameInputDeviceInfo::supportedInput, at the offset the game's own code reads it.
static uint32_t* supportedInput(void* device) {
    using GetInfo     = long (*)(void*, uint8_t const**);
    uint8_t const* info = nullptr;
    auto           fn   = reinterpret_cast<GetInfo>((*reinterpret_cast<void***>(device))[3]);
    return fn(device, &info) >= 0 && info ? const_cast<uint32_t*>(reinterpret_cast<uint32_t const*>(info + 0x60)) : nullptr;
}

constexpr uint32_t kKindKeyboardMouse = 0x30, kKindGamepad = 0x40000;

// Some gamepads (e.g. 8BitDo) also report mouse input. The game always gives keyboard/mouse devices to player 1,
// so such a pad could never get a player of its own. Treat a gamepad as a gamepad only.
static bool asPureGamepad(void* device) {
    auto kinds = supportedInput(device);
    if (!kinds || !(*kinds & kKindGamepad)) return false;
    if (*kinds & kKindKeyboardMouse) {
        note("device {}: gamepad also reports keyboard/mouse ({:X}), ignoring that", deviceName(device), *kinds);
        ll::memory::modify(kinds, sizeof(*kinds), [&] { *kinds &= ~kKindKeyboardMouse; });
    }
    return true;
}

using DeviceCallback = void (*)(uint64_t, void*, void*, uint64_t, uint32_t, uint32_t);
static DeviceCallback gDeviceCallback = nullptr;

// ---- Player picker: a connected controller can take over any player without a controller, or start a new one ----

struct LastConnect {
    uint64_t token = 0, time = 0;
    void*    context = nullptr;
};
static LastConnect gLastConnect;

static void fireConnection(IClientInstance& client, bool connected, int id) {
    auto input = client.getMinecraftInput();
    if (!input || !input->mInputHandler.get()) return;
    if (auto& callback = input->mInputHandler.get()->mControllerConnectionStateChangeHandler.get())
        callback(client, connected, id);
}

using ClientMap = std::map<SubClientId, std::shared_ptr<IClientInstance>>;

static IClientInstance* clientWithController(ClientMap const& clients, int id) {
    for (auto& [sub, client] : clients)
        if (client && client->getControllerId() == id) return client.get();
    return nullptr;
}

// The controller in slot `from` moves to slot `to` (the two slots trade their gamepads).
// `announceLoss`: tell the player left in `from` that their controller is gone (false when only giving back a
// controller that was lent for a menu).
static void moveController(IMinecraftGame& game, int from, int to, bool announceLoss = true) {
    auto handler = controllerHandler();
    if (!handler) return;
    {
        std::lock_guard lock(*handler->mPlayerInputsLock);
        auto&           inputs = *handler->mPlayerInputs;
        if (!inputs.contains(from) || !inputs.contains(to)) return note("picker: no slot {} or {}", from, to);
        std::swap(inputs.at(from).mGamepadSupport, inputs.at(to).mGamepadSupport);
        for (int id : {from, to})
            inputs.at(id).mConnectionState = gamepadsOf(inputs.at(id)).empty() ? PlayerInputConnectionState::Disconnected
                                                                               : PlayerInputConnectionState::Connected;
    }
    note("picker: controller moved from slot {} to slot {}", from, to);
    auto clients = game.getClientInstanceMap();
    if (auto c = clientWithController(clients, to)) fireConnection(*c, true, to);
    if (auto c = clientWithController(clients, from); c && announceLoss) fireConnection(*c, false, from);
}

// The controller in slot `from` gets a slot of its own; the game then shows its "press to join" prompt.
static void newPlayerFor(IMinecraftGame& game, int from, void* device) {
    auto handler = controllerHandler();
    if (!handler || !gLastConnect.context) return;
    std::set<int> players;
    {
        std::lock_guard idsLock(gPlayerIdsLock);
        players = gPlayerIds;
    }
    // The game hands a connecting controller to the first slot that has an Xbox user and no controller (player 1's
    // slot, or the one it just left). Hide every user during the reconnect, and lend player 1's to an empty slot
    // without a player if there is one, so the controller lands there or in a new slot.
    std::vector<std::pair<void**, void*>> hidden;
    {
        std::lock_guard lock(*handler->mPlayerInputsLock);
        auto&           inputs = *handler->mPlayerInputs;
        if (!inputs.contains(from)) return;
        // shortcut: the slot's gamepad list object is left allocated (its destructor has no address in LeviLamina);
        // a few bytes and one device reference per "New player" choice.
        (void)inputs.at(from).mGamepadSupport.release();
        inputs.at(from).mConnectionState = PlayerInputConnectionState::Disconnected;
        void* user = nullptr;
        for (auto& [id, input] : inputs) {
            auto u = reinterpret_cast<void**>(&input.mMappedUser);
            if (id == 0) user = *u;
            if (*u) hidden.emplace_back(u, *u), *u = nullptr;
        }
        for (auto& [id, input] : inputs)
            if (id != from && user && !players.contains(id) && gamepadsOf(input).empty()) {
                auto u = reinterpret_cast<void**>(&input.mMappedUser);
                hidden.emplace_back(u, nullptr);
                *u = user;
                break;
            }
    }
    tHideUser = true;
    gDeviceCallback(gLastConnect.token, gLastConnect.context, device, gLastConnect.time, 1, 0); // connect it again
    tHideUser = false;
    {
        std::lock_guard lock(*handler->mPlayerInputsLock);
        for (auto& [u, value] : hidden) *u = value;
    }
    note("picker: controller left slot {} for a new player", from);
    auto clients = game.getClientInstanceMap();
    if (auto c = clientWithController(clients, from)) fireConnection(*c, false, from);
}

static int slotOfDevice(void* device) {
    auto handler = controllerHandler();
    if (!handler) return -1;
    std::lock_guard lock(*handler->mPlayerInputsLock);
    for (auto& [id, input] : *handler->mPlayerInputs)
        for (auto d : gamepadsOf(input))
            if (d == device) return id;
    return -1;
}

// Which saved local player (sub id) the next joining player becomes; -1 lets the game pick the lowest free one.
static std::atomic<int> gWantedSub = -1;

using TryGetSubId = bool (*)(void*, int, SubClientId&, bool);
static TryGetSubId gTryGetSubId = nullptr;

// MinecraftGame::tryGetIdForNewSubClient: hand out the wanted saved player instead of the lowest free one.
static bool tryGetSubIdDetour(void* game, int controllerId, SubClientId& out, bool initialCheck) {
    bool ok   = gTryGetSubId(game, controllerId, out, initialCheck);
    int  want = gWantedSub;
    if (ok && want > 0) { // the join itself asks with initialCheck set too
        auto clients = static_cast<IMinecraftGame*>(game)->getClientInstanceMap();
        if (!clients.contains(SubClientId(want))) out = SubClientId(want);
    }
    return ok;
}

// "SnakyWarrior (2)": the game names local players after the gamertag without its number suffix.
static std::string localName(ClientMap const& clients, int sub) {
    if (sub == 0)
        if (auto it = clients.find(SubClientId::PrimaryClient); it != clients.end() && it->second && it->second->getLocalPlayer())
            return it->second->getLocalPlayer()->getRealName();
    std::string base;
    for (auto& [id, client] : clients) {
        if (!client || !client->getLocalPlayer()) continue;
        auto name = client->getLocalPlayer()->getRealName();
        if (id != SubClientId::PrimaryClient) {
            if (auto cut = name.rfind(" ("); cut != std::string::npos) return fmt::format("{} ({})", name.substr(0, cut), sub);
        } else {
            base = name;
            while (!base.empty() && std::isdigit(static_cast<unsigned char>(base.back()))) base.pop_back();
        }
    }
    return fmt::format("{} ({})", base, sub);
}

static Player* serverPlayer(SubClientId sub) {
    Player* found = nullptr;
    if (auto level = ll::service::getLevel())
        level->forEachPlayer([&](Player& player) {
            if (player.getClientSubId() != sub) return true;
            found = &player;
            return false;
        });
    return found;
}

// The mod's menus are the game's own forms, shown on one player's part of the screen. Player buttons carry the
// game's name tag picture. `done` gets the chosen index, or -1 when closed, on the client thread.
// Button texts the resource pack draws specially (see its server_form.json): a square button laid over the right end
// of the row above it (remove that player / take player 1's controller), a row that leaves room for that square,
// and the green closing button.
static constexpr char const* kKickButton = "[x]";
static constexpr char const* kPadButton  = "[p]";
static constexpr char const* kRoomForSquare = " "; // a leading space: not seen, but the UI can test for it
static constexpr char const* kDoneButton = "Done";

static void showMenu(
    SubClientId                     who,
    std::string const&              text,
    std::vector<std::string> const& buttons,
    std::vector<std::string> const& pictures, // per button: a texture path, or "" for none
    std::function<void(int)>        done,
    std::string const&              title
) {
    auto player = serverPlayer(who);
    if (!player || buttons.empty() || menuOpen(int(who)))
        return note("menu for player {} not shown (player found: {}, already open: {})", int(who) + 1, bool(player),
                    menuOpen(int(who)));
    gMenus[int(who)] = std::chrono::steady_clock::now();
    ll::form::SimpleForm form(title, text);
    for (size_t i = 0; i < buttons.size(); ++i)
        if (i < pictures.size() && !pictures[i].empty()) form.appendButton(buttons[i], pictures[i], "path");
        else form.appendButton(buttons[i]);
    form.sendTo(*player, [done, sub = int(who)](Player&, int pick, ll::form::FormCancelReason) {
        ll::thread::ClientThreadExecutor::getDefault().execute([done, pick, sub] {
            gMenus.erase(sub);
            done(pick);
        });
    });
}

// A short line above that player's hotbar, so they see what a menu choice did.
static void tell(SubClientId sub, std::string const& text) {
    if (auto player = serverPlayer(sub)) TextPacketPayload::createJukeboxPopup(text, {}).sendTo(*player);
}

// Player 1 goes back to keyboard and mouse; the controller gets a slot of its own and waits for start to be pressed,
// then asks who it is for. The game's "connect your controller" screen for player 1 is closed again.
static void freeFirstPad(IMinecraftGame& game, void* pad) {
    note("controller taken from player 1");
    newPlayerFor(game, 0, pad);
    tell(SubClientId::PrimaryClient, "Press start on your controller to choose who plays with it");
    ll::thread::ClientThreadExecutor::getDefault().executeAfter(
        [] {
            if (auto c = ll::service::getClientInstance(); c && c->getTopScreenName().find("gamepad_disconnected") != std::string::npos)
                c->getMainSceneStack()->schedulePopScreen(1);
        },
        std::chrono::milliseconds(300)
    );
}

// Player pictures: P1-P4 (the same as their part of the screen) for players with a controller, else the name tag.
static constexpr char const* kNameTag = "textures/items/name_tag";
// The picture takes that player's own colour (the one the game gives them, e.g. on the locator bar), as the nearest of
// 24 hues, or white when it has no colour.
static std::string badge(int sub) {
    int shade = 24;
    if (auto player = serverPlayer(SubClientId(sub)))
        if (auto level = ll::service::getLevel()) {
            mce::Color c  = level->getPlayerColor(*player);
            float      hi = std::max({c.r, c.g, c.b}), lo = std::min({c.r, c.g, c.b});
            if (hi - lo > 0.15f) {
                float h = hi == c.r ? (c.g - c.b) / (hi - lo) : hi == c.g ? 2 + (c.b - c.r) / (hi - lo) : 4 + (c.r - c.g) / (hi - lo);
                h       = std::fmod(h / 6 + 1, 1.f);
                shade   = int(std::lround(h * 24)) % 24;
            }
        }
    return fmt::format("textures/splitscreen/p{}_{}", sub + 1, shade);
}

// F7 or the pause menu's "Split screen" button: layout, and removing players.
// The player whose pause menu is open, else player 1.
static int pausedPlayer() {
    auto client = ll::service::getClientInstance();
    if (client)
        for (auto& [sub, other] : client->getMinecraftGame().getClientInstanceMap())
            if (other && other->getTopScreenName().find("pause") != std::string::npos) return int(sub);
    return 0;
}

static void rejoinAs(SubClientId leaving, int controllerId, int sub, int tries);

static void openSettings(int who) {
    auto client = ll::service::getClientInstance();
    if (!client) return;
    if (who < 0) who = pausedPlayer();
    if (menuOpen(who)) return;
    auto& game     = client->getMinecraftGame();
    auto  clients  = game.getClientInstanceMap();
    bool  vertical = int(client->getOptions().getSplitscreenDirection()) == 1;

    std::set<int> withPad;
    void*         firstPad = nullptr; // player 1's controller, if they have one
    if (auto handler = controllerHandler()) {
        std::lock_guard lock(*handler->mPlayerInputsLock);
        for (auto& [id, input] : *handler->mPlayerInputs)
            if (auto pads = gamepadsOf(input); !pads.empty()) {
                withPad.insert(id);
                if (id == 0) firstPad = pads.front();
            }
    }
    enum class Act { Horizontal, Vertical, Swap, Kick, FreePad, Join, Done, Nothing };
    struct Item {
        Act act;
        int sub;
    };
    // the layout as two options, the chosen one marked; section titles are drawn by the resource pack
    std::vector<Item>        items{{Act::Nothing, -1}, {Act::Horizontal, -1}, {Act::Vertical, -1}, {Act::Nothing, -1}};
    std::vector<std::string> buttons{"[h:Layout]", "Top and bottom", "Side by side", "[h:Players]"};
    std::vector<std::string> pictures{
        "",
        vertical ? "textures/splitscreen/radio_off" : "textures/splitscreen/radio_on",
        vertical ? "textures/splitscreen/radio_on" : "textures/splitscreen/radio_off",
        ""
    };
    for (int sub = 0; sub <= 3; ++sub) {
        auto it   = clients.find(SubClientId(sub));
        auto name = localName(clients, sub);
        if (it != clients.end() && it->second) { // playing: pick to play as them, square to remove / free controller
            int  controller = it->second->getControllerId();
            auto status     = sub == 0 ? "Player 1" : withPad.contains(controller) ? "Playing" : "No controller";
            items.push_back({Act::Swap, sub});
            buttons.push_back(fmt::format("{}{}{}", kRoomForSquare, name, sub == who ? " §3(You)" : ""));
            pictures.push_back(withPad.contains(controller) ? badge(sub) : kNameTag);
            items.push_back({Act::Nothing, -1}); // the status tag, drawn on the row above
            buttons.push_back(fmt::format("[stag:{}]", status));
            pictures.emplace_back();
            items.push_back({sub == 0 ? Act::FreePad : Act::Kick, sub});
            buttons.push_back(sub == 0 ? kPadButton : kKickButton);
            pictures.push_back(sub == 0 ? "textures/ui/controller_glyph" : "");
        } else if (who != 0) { // a saved player who is not playing: this player switches to them
            items.push_back({Act::Join, sub});
            buttons.push_back(name);
            pictures.push_back(kNameTag);
            items.push_back({Act::Nothing, -1});
            buttons.push_back("[tag:Not playing]");
            pictures.emplace_back();
        }
    }
    items.push_back({Act::Done, -1});
    buttons.push_back(kDoneButton);
    pictures.emplace_back();

    showMenu(SubClientId(who), "", buttons, pictures, [items, who, firstPad](int pick) {
        auto client = ll::service::getClientInstance();
        if (!client || pick < 0 || pick >= int(items.size())) return;
        auto& game    = client->getMinecraftGame();
        auto  clients = game.getClientInstanceMap();
        auto  item    = items[pick];
        auto  mine    = clients.find(SubClientId(who));
        int   myPad   = mine != clients.end() && mine->second ? mine->second->getControllerId() : -1;
        switch (item.act) {
        case Act::Horizontal:
        case Act::Vertical:
            client->getOptions().setSplitscreenDirection(item.act == Act::Vertical ? 1 : 0);
            return openSettings(who); // show the new choice
        case Act::Swap: // this controller plays as that player; theirs (if any) comes here
            if (auto it = clients.find(SubClientId(item.sub)); item.sub != who && it != clients.end() && it->second && myPad >= 0) {
                moveController(game, myPad, it->second->getControllerId());
                tell(SubClientId(item.sub), fmt::format("You are now {}", localName(clients, item.sub)));
            }
            return;
        case Act::Kick:
            if (auto it = clients.find(SubClientId(item.sub)); it != clients.end() && it->second) {
                note("settings: player {} removed", item.sub + 1);
                tell(SubClientId(who), fmt::format("{} left the game", localName(clients, item.sub)));
                it->second->requestLeaveGameAsync();
            }
            return;
        case Act::FreePad: // player 1 goes back to keyboard and mouse; the controller waits for start to be pressed
            if (firstPad) {
                freeFirstPad(game, firstPad);
            }
            return;
        case Act::Join:
            if (who > 0 && myPad >= 0) rejoinAs(SubClientId(who), myPad, item.sub, 0);
            return;
        default: return;
        }
    }, "Split screen");
}

static void joinAs(IMinecraftGame& game, int controllerId, int sub) {
    gWantedSub = sub;
    gNoMenuFor.insert(sub);
    note("picker: controller {} joins as saved player {}", controllerId, sub);
    tOurJoin = true;
    game.addSubClientInstanceAndAssociateToUser(controllerId);
    tOurJoin = false;
    gWantedSub = -1;
}

// Player `leaving` leaves, then its controller joins again as saved player `sub`.
static void rejoinAs(SubClientId leaving, int controllerId, int sub, int tries = 0) {
    auto client = ll::service::getClientInstance();
    if (!client) return;
    auto& game    = client->getMinecraftGame();
    auto  clients = game.getClientInstanceMap();
    if (tries == 0 && clients.contains(leaving)) clients.at(leaving)->requestLeaveGameAsync();
    if (clients.contains(leaving) && tries < 40) { // wait up to 20 s for the player to be gone
        ll::thread::ClientThreadExecutor::getDefault().executeAfter(
            [=] { rejoinAs(leaving, controllerId, sub, tries + 1); },
            std::chrono::milliseconds(500)
        );
        return;
    }
    joinAs(game, controllerId, sub);
}

// "Choose your player" closes by itself 10 s after it appears on that player's screen (it can arrive a few seconds
// after it was sent); nobody joins then and the stand-in leaves.
static void watchChoice(SubClientId owner, std::chrono::steady_clock::time_point sent, std::chrono::steady_clock::time_point seen) {
    auto it = gMenus.find(int(owner));
    if (it == gMenus.end() || it->second != sent) return; // answered
    auto now    = std::chrono::steady_clock::now();
    auto client = ll::service::getClientInstance();
    if (!client) return;
    auto clients = client->getMinecraftGame().getClientInstanceMap();
    auto c       = clients.find(owner);
    if (c == clients.end() || !c->second) return;
    if (seen == std::chrono::steady_clock::time_point{} && c->second->getTopScreenName().find("third_party_server_screen") != std::string::npos)
        seen = now;
    if (seen != std::chrono::steady_clock::time_point{} && now - seen >= std::chrono::seconds(10)) {
        gMenus.erase(it);
        note("picker: nobody chosen for player {} in 10 s", int(owner) + 1);
        quietFor(std::chrono::seconds(30));
        c->second->requestLeaveGameAsync();
        return;
    }
    ll::thread::ClientThreadExecutor::getDefault().executeAfter([=] { watchChoice(owner, sent, seen); },
                                                                std::chrono::milliseconds(200));
}

// Show the menu on player `owner`'s screen. `fresh`: the player just joined (rather than picking up a controller).
static void offerPicker(SubClientId owner, bool fresh) {
    auto client = ll::service::getClientInstance();
    if (!client) return;
    auto& game    = client->getMinecraftGame();
    auto  clients = game.getClientInstanceMap();
    if (!clients.contains(owner) || !clients.at(owner)) return;
    int controller = clients.at(owner)->getControllerId();

    std::set<int> withPad;
    if (auto handler = controllerHandler()) {
        std::lock_guard lock(*handler->mPlayerInputsLock);
        for (auto& [id, input] : *handler->mPlayerInputs)
            if (!gamepadsOf(input).empty()) withPad.insert(id);
    }
    enum class Kind { Keep, TakeOver, Offline, NewPlayer };
    struct Choice {
        Kind        kind;
        int         value; // controller id (TakeOver) or sub id (Offline)
        SubClientId sub;
    };
    // One button per player in player order (player 1 first): this controller's own player, players without a
    // controller, and saved players who are not playing.
    std::vector<Choice> choices{{Kind::Keep, 0, owner}};
    for (auto& [sub, other] : clients)
        if (sub != owner && other && !withPad.contains(other->getControllerId()))
            choices.push_back({Kind::TakeOver, other->getControllerId(), sub});
    for (int sub = 1; sub <= 3; ++sub)
        if (!clients.contains(SubClientId(sub))) choices.push_back({Kind::Offline, sub, SubClientId(sub)});
    std::stable_sort(choices.begin(), choices.end(), [](Choice const& a, Choice const& b) { return a.sub < b.sub; });
    std::vector<std::string> buttons;
    std::vector<std::string> pictures;
    std::vector<int>         toChoice; // form button -> choice; -1 for the status tags drawn on the row above
    for (int i = 0; i < int(choices.size()); ++i) {
        auto& c       = choices[i];
        bool  hasPad = c.kind == Kind::Keep && !fresh; // only this controller's own player has one in this list
        buttons.push_back(localName(clients, int(c.sub)));
        pictures.push_back(hasPad ? badge(int(c.sub)) : kNameTag);
        toChoice.push_back(i);
        // the stand-in of a new controller is not really playing yet
        buttons.push_back(c.kind == Kind::Offline || (c.kind == Kind::Keep && fresh) ? "[tag:Not playing]"
                          : c.kind == Kind::TakeOver                                 ? "[tag:No controller]"
                                                                                     : "[tag:Playing]");
        pictures.emplace_back();
        toChoice.push_back(-1);
    }
    if (!fresh && clients.size() < 4) {
        toChoice.push_back(int(choices.size()));
        choices.push_back({Kind::NewPlayer, 0, owner});
        buttons.push_back("New player");
        pictures.emplace_back();
    }
    int const keep = int(std::find_if(choices.begin(), choices.end(), [](Choice const& c) { return c.kind == Kind::Keep; })
                         - choices.begin());
    if (choices.size() < 2) return;
    note("picker: menu for player {} ({} choices)", int(owner) + 1, choices.size());
    showMenu(owner, "Who is playing with this controller?", buttons, pictures, [=](int pick) {
        pick        = pick >= 0 && pick < int(toChoice.size()) ? toChoice[pick] : -1;
        auto client = ll::service::getClientInstance();
        if (fresh) {
            if (pick == keep || pick < 0 || pick >= int(choices.size())) { // kept: the stand-in is the chosen player
                if (auto player = serverPlayer(owner)) hideStandIn(*player, false);
                if (auto c = ll::service::getClientInstance())
                    tell(owner, fmt::format("You are {}", localName(c->getMinecraftGame().getClientInstanceMap(), int(owner))));
                quietFor(std::chrono::seconds(10)); // its skin line can still come
            } else quietFor(std::chrono::seconds(30)); // the stand-in leaves and the chosen player comes in quietly
        }
        if (!client || pick == keep || pick < 0 || pick >= int(choices.size())) return; // closed, or kept
        auto& game   = client->getMinecraftGame();
        auto& choice = choices[pick];
        switch (choice.kind) {
        case Kind::TakeOver:
            moveController(game, controller, choice.value);
            tell(choice.sub, fmt::format("You are now {}", localName(game.getClientInstanceMap(), int(choice.sub))));
            if (fresh) { // the player made for this controller is not needed any more
                auto now = game.getClientInstanceMap();
                if (now.contains(owner)) now.at(owner)->requestLeaveGameAsync();
            }
            break;
        case Kind::Offline:
            if (fresh) rejoinAs(owner, controller, choice.value);
            else {
                // keep the current player (it waits for a controller) and give this one a slot of its own first
                note("picker: playing saved player {} needs the controller in a slot of its own", choice.value);
                if (auto device = [&]() -> void* {
                        auto handler = controllerHandler();
                        if (!handler) return nullptr;
                        std::lock_guard lock(*handler->mPlayerInputsLock);
                        auto&           inputs = *handler->mPlayerInputs;
                        auto            pads   = inputs.contains(controller) ? gamepadsOf(inputs.at(controller)) : std::vector<void*>{};
                        return pads.empty() ? nullptr : pads.front();
                    }()) {
                    gWantedSub = choice.value;
                    gNoMenuFor.insert(choice.value);
                    newPlayerFor(game, controller, device);
                    ll::thread::ClientThreadExecutor::getDefault().executeAfter(
                        [device] {
                            int slot = slotOfDevice(device);
                            if (auto c = ll::service::getClientInstance(); c && slot >= 0) {
                                tOurJoin = true;
                                c->getMinecraftGame().addSubClientInstanceAndAssociateToUser(slot);
                                tOurJoin = false;
                            }
                            gWantedSub = -1;
                        },
                        std::chrono::milliseconds(500)
                    );
                }
            }
            break;
        case Kind::NewPlayer: {
            auto handler = controllerHandler();
            void* device = nullptr;
            if (handler) {
                std::lock_guard lock(*handler->mPlayerInputsLock);
                auto&           inputs = *handler->mPlayerInputs;
                if (inputs.contains(controller) && !gamepadsOf(inputs.at(controller)).empty())
                    device = gamepadsOf(inputs.at(controller)).front();
            }
            if (device) newPlayerFor(game, controller, device);
            break;
        }
        default: break;
        }
    }, fresh ? "Choose your player" : "Who is playing?");
    if (fresh && gMenus.contains(int(owner))) watchChoice(owner, gMenus.at(int(owner)), {});
}

// A new controller joins at once as the first free saved player, and its own part of the screen (black, see the
// resource pack's server_form.json) asks who is playing; picking someone else swaps that player in.
static void startJoin(IMinecraftGame& game, int slot) {
    auto clients = game.getClientInstanceMap();
    int  sub     = 1;
    while (sub <= 3 && clients.contains(SubClientId(sub))) ++sub;
    if (sub > 3) return;
    gFreshSub = sub;
    gFreshAt  = std::chrono::steady_clock::now();
    quietFor(std::chrono::seconds(30));
    joinAs(game, slot, sub);
}

// IMinecraftGame::addSubClientInstanceAndAssociateToUser, as called by the game's join prompt.
static void joinRequestDetour(void* self, int controllerId) {
    auto original = reinterpret_cast<void (*)(void*, int)>(gCfg["addSubClientInstanceAndAssociateToUser"].origin);
    if (tOurJoin) return original(self, controllerId);
    if (!ll::service::getLevel()) {
        if (controllerId <= 0) return original(self, controllerId);
        note("controller {} asked to join before the world: asking in the world", controllerId);
        gAskLater.insert(controllerId);
        return;
    }
    if (gFreshSub >= 0) return; // someone is joining already
    startJoin(*static_cast<IMinecraftGame*>(self), controllerId);
}

// A controller was connected: offer the menu to the player that got it.
static void onControllerConnected(void* device) {
    auto client = ll::service::getClientInstance();
    int  slot   = slotOfDevice(device); // also proves the device is still in the game's table
    if (!client || !ll::service::getLevel() || slot < 0) return;
    auto& game    = client->getMinecraftGame();
    auto  clients = game.getClientInstanceMap();
    for (auto& [sub, other] : clients)
        if (other && other->getControllerId() == slot) return offerPicker(sub, false);
    // no player yet: pressing start on it opens the menu (see joinRequestDetour)
}

static void deviceCallbackDetour(uint64_t token, void* context, void* device, uint64_t time, uint32_t now, uint32_t before) {
    bool pad = device && gFindUser && asPureGamepad(device);
    note("device {}: status {:X} (was {:X}){}", device ? deviceName(device) : "?", now, before, pad ? ", gamepad" : "");
    // A connecting gamepad should fill the lowest empty player slot. The game only reuses a slot that has an Xbox user,
    // and the slots it made for players 2-4 have none (their user is hidden above), so it kept opening new slots until
    // it ran past the 4 controllers it supports and crashed. Lend player 1's user to the lowest empty slot meanwhile.
    auto       handler = static_cast<GameControllerHandler_GameCore*>(context);
    void**     lentTo  = nullptr;
    void**     hiddenFirst = nullptr;
    void*      user    = nullptr;
    bool const connecting = pad && handler && (now & 1) && !(before & 1); // GameInputDeviceConnected
    if (connecting) {
        std::lock_guard lock(*handler->mPlayerInputsLock);
        auto&           inputs = *handler->mPlayerInputs;
        // The user handle is the first field of XsapiHandle (the game's own code tests it there).
        auto userOf = [](GameCorePlayerInput& in) { return reinterpret_cast<void**>(&in.mMappedUser); };
        if (inputs.contains(0)) user = *userOf(inputs.at(0));
        // In a world player 1 keeps keyboard and mouse; a controller only becomes theirs if picked in the menu.
        if (gInWorld && user && inputs.contains(0)) {
            hiddenFirst = userOf(inputs.at(0));
            *hiddenFirst = nullptr;
        }
        std::set<int> players;
        {
            std::lock_guard idsLock(gPlayerIdsLock);
            players = gPlayerIds;
        }
        // A player who lost their controller first, then any empty slot.
        for (int pass = 0; pass < 2 && !lentTo; ++pass)
            for (auto& [id, input] : inputs)
                if (id > 0 && user && !*userOf(input) && gamepadsOf(input).empty() && (pass == 1 || players.contains(id))) {
                    lentTo  = userOf(input);
                    *lentTo = user;
                    note("device {}: reusing empty controller slot {}", deviceName(device), id);
                    break;
                }
    }
    tHideUser = pad;
    gDeviceCallback(token, context, device, time, now, before);
    tHideUser = false;
    if (lentTo || hiddenFirst) {
        std::lock_guard lock(*handler->mPlayerInputsLock);
        if (lentTo && *lentTo == user) *lentTo = nullptr;
        if (hiddenFirst && !*hiddenFirst) *hiddenFirst = user;
    }
    if (connecting) {
        gLastConnect = {token, time, context};
        ll::thread::ClientThreadExecutor::getDefault().executeAfter(
            [device] { onControllerConnected(device); },
            std::chrono::milliseconds(500)
        );
    }
}

// LeviLamina remembers the *last* ClientInstance created as "the" client. Players 2-4 get ClientInstances too, so
// after one joined it pointed at that player, and once the player left it pointed at freed memory: leaving and
// re-entering a world then crashed inside LeviLamina (and this mod used the same stale pointer). Keep it on player 1.
static std::atomic<ClientInstance*>* gLlClient = nullptr;

// LeviLamina's getClientInstance() is "mov rax, [rip+x]; ret" around that variable; find x.
static std::atomic<ClientInstance*>* findLlClientVar() {
    auto code = reinterpret_cast<uint8_t const*>(&ll::service::getClientInstance);
    for (int i = 0; i < 4; ++i) { // through jmp stubs
        if (code[0] == 0xE9) code += 5 + *reinterpret_cast<int32_t const*>(code + 1);
        else if (code[0] == 0xFF && code[1] == 0x25)
            code = *reinterpret_cast<uint8_t const* const*>(code + 6 + *reinterpret_cast<int32_t const*>(code + 2));
        else break;
    }
    for (int p = 0; p < 48; ++p) // mov r64, [rip+disp32]
        if ((code[p] & 0xFB) == 0x48 && code[p + 1] == 0x8B && (code[p + 2] & 0xC7) == 0x05)
            return reinterpret_cast<std::atomic<ClientInstance*>*>(
                const_cast<uint8_t*>(code + p + 7 + *reinterpret_cast<int32_t const*>(code + p + 3))
            );
    return nullptr;
}

LL_AUTO_TYPE_INSTANCE_HOOK(
    KeepPrimaryClient,
    ll::memory::HookPriority::Highest, // outside LeviLamina's own hook, so this runs after it
    ClientInstance,
    &ClientInstance::$ctor,
    void*,
    ClientInstanceArguments&& args
) {
    ClientInstance* primary = gLlClient ? gLlClient->load() : nullptr;
    auto            result  = origin(std::move(args));
    if (primary) gLlClient->store(primary);
    return result;
}

bool MyMod::load() {
    note("SplitScreenMod loading");
    gLlClient = findLlClientVar(); // checked in enable(), once the client exists
    readConfig(gWanted);
    if (auto cb = findDeviceCallback()) {
        ll::memory::hook(reinterpret_cast<ll::memory::FuncPtr>(cb), reinterpret_cast<ll::memory::FuncPtr>(&deviceCallbackDetour),
                         reinterpret_cast<ll::memory::FuncPtr*>(&gDeviceCallback), ll::memory::HookPriority::Normal);
        note("device callback at rva {:X}", cb - gameBase());
        if (auto lookup = findUserLookup(cb)) {
            ll::memory::hook(reinterpret_cast<ll::memory::FuncPtr>(lookup), reinterpret_cast<ll::memory::FuncPtr>(&findUserDetour),
                             reinterpret_cast<ll::memory::FuncPtr*>(&gFindUser), ll::memory::HookPriority::Normal);
            note("user lookup at rva {:X}", lookup - gameBase());
        } else note("user lookup not found: extra controllers may all control player 1");
    } else note("device callback not found");
    // AppPlatform does not exist yet at load; follow it from creation until the game is up.
    std::thread([] {
        for (int i = 0; i < 60000; ++i) {
            if (ll::service::getAppPlatform()) hookPlatform();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }).detach();
    return true;
}

bool MyMod::enable() {
    note("SplitScreenMod enabling");
    if (!gLlClient || !ll::service::getClientInstance() || gLlClient->load() != ll::service::getClientInstance().as_ptr()) {
        note("LeviLamina client pointer not found: leaving a world with several players may crash");
        gLlClient = nullptr;
    }
    auto& hooks  = gWanted;
    hookPlatform();
    auto client = ll::service::getClientInstance();
    if (!gPlatformHooked && hooks.contains("allowSplitScreen") || !client) {
        note("ERROR: game not ready (platform hooked: {}, client: {})", gPlatformHooked.load(), bool(client));
        return false;
    }
    IMinecraftGame& game = client->getMinecraftGame();
    if (auto slot = findSlot(&game, &IMinecraftGame::isSplitscreenJoinEnabled, "isSplitscreenJoinEnabled"))
        gJoinFn = reinterpret_cast<uintptr_t>(*slot);
    if (auto slot = findSlot(&game, &IMinecraftGame::canAddSplitscreenSubClient, "canAddSplitscreenSubClient")) {
        auto body = reinterpret_cast<uint8_t const*>(*slot);
        for (int p = 0; p < 64 && !gTryGetSubId; ++p)
            if (body[p] == 0xE8) {
                auto target = reinterpret_cast<uintptr_t>(body + p + 5) + *reinterpret_cast<int32_t const*>(body + p + 1);
                ll::memory::hook(reinterpret_cast<ll::memory::FuncPtr>(target), reinterpret_cast<ll::memory::FuncPtr>(&tryGetSubIdDetour),
                                 reinterpret_cast<ll::memory::FuncPtr*>(&gTryGetSubId), ll::memory::HookPriority::Normal);
                note("sub id choice at rva {:X}", target - gameBase());
            }
        if (!gTryGetSubId) note("sub id choice not found: picking a saved player will not work");
    }
    hookSlot(&game, &IMinecraftGame::addSubClientInstanceAndAssociateToUser, fn(&joinRequestDetour),
             "addSubClientInstanceAndAssociateToUser");
    if (hooks.contains("isSplitscreenJoinEnabled"))
        hookSlot(&game, &IMinecraftGame::isSplitscreenJoinEnabled, fn(&joinEnabledDetour), "isSplitscreenJoinEnabled");
    if (hooks.contains("canAddSplitscreenSubClient"))
        hookSlot(&game, &IMinecraftGame::canAddSplitscreenSubClient, fn(&canAddSubClientDetour), "canAddSplitscreenSubClient");
    ll::thread::ClientThreadExecutor::getDefault().execute(pollTrigger);
    note("SplitScreenMod enabled ({} hooks)", gHooks.size());
    return true;
}

bool MyMod::disable() {
    for (auto& h : gHooks) ll::memory::modify(h.slot, sizeof(*h.slot), [&] { *h.slot = h.original; });
    gHooks.clear();
    gPlatformHooked = false;
    return true;
}

} // namespace my_mod

LL_REGISTER_MOD(my_mod::MyMod, my_mod::MyMod::getInstance());
