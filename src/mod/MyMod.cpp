#include "mod/MyMod.h"

#include <algorithm>
#include <atomic>
#include <chrono>
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
#include "mc/client/game/IClientInstance.h"
#include "mc/client/game/IMinecraftGame.h"
#include "mc/client/input/MinecraftInputHandler.h"
#include "mc/deps/application/gamecore/Platform_GameCore.h"
#include "mc/deps/entry/src_gamecore/input/GameCorePlayerInput.h"
#include "mc/gamecore/AppPlatform_GameCore.h"
#include "mc/gamecore/GameControllerHandler_GameCore.h"
#include "mc/deps/input/InputHandler.h"
#include "mc/locale/Localization.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"
#include "ll/api/form/SimpleForm.h"
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
static ll::memory::FuncPtr* hookSlot(void* object, F method, ll::memory::FuncPtr detour, char const* name) {
    auto slot = findSlot(object, method, name);
    if (!slot) return nullptr;
    if (*slot == detour) return slot;
    Hooked h{slot, *slot};
    gCfg[name].origin = h.original; // detours read this, so set it before the slot goes live
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
static GameControllerHandler_GameCore* controllerHandler();

static void runTrigger(std::string const& cmd, int arg) {
    auto client = ll::service::getClientInstance();
    if (!client) return note("trigger {}: no client instance", cmd);
    auto& game = client->getMinecraftGame();
    if (cmd == "join") {
        note("adding player on controller {}", arg);
        game.addSubClientInstanceAndAssociateToUser(arg);
    }
    if (cmd == "pads") dumpPlayers(game);
    // prototype: "form N" shows a test form to the Nth server-side player; "swap A B" swaps the gamepads of two
    // controller ids in the game's table.
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

static std::atomic<int> gSecondsLeft = -1; // shown in the "controller lost" prompt; -1 when nobody is leaving

static void checkControllers(IMinecraftGame& game) {
    auto now  = std::chrono::steady_clock::now();
    int  left = -1;
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
    auto       window = GetForegroundWindow();
    DWORD      pid    = 0;
    GetWindowThreadProcessId(window, &pid);
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

static void deviceCallbackDetour(uint64_t token, void* context, void* device, uint64_t time, uint32_t now, uint32_t before) {
    bool pad = device && gFindUser && asPureGamepad(device);
    note("device {}: status {:X} (was {:X}){}", device ? deviceName(device) : "?", now, before, pad ? ", gamepad" : "");
    tHideUser = pad;
    gDeviceCallback(token, context, device, time, now, before);
    tHideUser = false;
}

bool MyMod::load() {
    note("SplitScreenMod loading");
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
