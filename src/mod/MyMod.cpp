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
#include "mc/client/game/IMinecraftGame.h"
#include "mc/client/input/MinecraftInputHandler.h"
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
static uintptr_t gInputFn = 0; // MinecraftInputHandler::_registerInputHandlers, set in load()

// Follow jmp rel32 / jmp [rip+x] stubs to the real function.
static uintptr_t followJumps(uintptr_t a) {
    for (int i = 0; i < 4; ++i) {
        auto c = reinterpret_cast<uint8_t const*>(a);
        if (c[0] == 0xE9) a += 5 + *reinterpret_cast<int32_t const*>(c + 1);
        else if (c[0] == 0xFF && c[1] == 0x25) a = *reinterpret_cast<uintptr_t const*>(a + 6 + *reinterpret_cast<int32_t const*>(c + 2));
        else break;
    }
    return a;
}

// True if the code contains "test ..., 0x40000" (GameInputKindGamepad): the code that hands new gamepads to players.
static bool testsGamepadKind(uint8_t const* body, size_t n) {
    for (size_t p = 6; p + 4 <= n; ++p)
        if (std::memcmp(body + p, "\x00\x00\x04\x00", 4) == 0
            && (body[p - 2] == 0xF7 || body[p - 3] == 0xF7 || body[p - 6] == 0xF7))
            return true;
    return false;
}

// Version-independent choice of which allowSplitScreen callers get "true": the splitscreen join check, the input
// setup that creates the "press a button to join" listener, and the GameInput code that would otherwise give every
// gamepad to player 1. Everything else keeps the PC answer,
// since forcing those callers broke the game.
static bool autoAllow(void* ret) {
    static std::map<void*, bool> verdict; // callers run often; decide() holds the lock
    if (auto it = verdict.find(ret); it != verdict.end()) return it->second;
    DWORD64 base = 0;
    auto    fn   = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(ret), &base, nullptr);
    if (!fn) return false;
    auto begin = base + fn->BeginAddress;
    bool yes   = begin == gJoinFn || begin == gInputFn
            || testsGamepadKind(reinterpret_cast<uint8_t const*>(begin), fn->EndAddress - fn->BeginAddress);
    if (gJoinFn) verdict[ret] = yes; // before enable() the join function is not known yet
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
static void runTrigger(std::string const& cmd, int arg) {
    auto client = ll::service::getClientInstance();
    if (!client) return note("trigger {}: no client instance", cmd);
    auto& game = client->getMinecraftGame();
    if (cmd == "join") {
        note("join {}: canAdd={} joinEnabled={}", arg, game.canAddSplitscreenSubClient(arg), game.isSplitscreenJoinEnabled());
        game.addSubClientInstanceAndAssociateToUser(arg);
        note("join {}: addSubClientInstanceAndAssociateToUser returned", arg);
    }
    note("info: active={} count={}", client->isSplitScreenActive(), client->getSplitScreenCount());
}

static void pollTrigger() {
    // F8 in the game window adds the next splitscreen player (controller ids 1..3).
    // Edge-detect the key: the "pressed since last call" bit of GetAsyncKeyState fired several joins for one press.
    static int  nextController = 1;
    static bool wasDown        = false;
    auto        window         = GetForegroundWindow();
    DWORD       pid            = 0;
    GetWindowThreadProcessId(window, &pid);
    bool down = (GetAsyncKeyState(VK_F8) & 0x8000) && pid == GetCurrentProcessId();
    if (down && !wasDown && nextController <= 3) runTrigger("join", nextController++);
    wasDown = down;
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

bool MyMod::load() {
    note("SplitScreenMod loading");
    readConfig(gWanted);
    {
        auto     method = &MinecraftInputHandler::$_registerInputHandlers;
        uintptr_t code;
        std::memcpy(&code, &method, sizeof(code));
        gInputFn = followJumps(code);
        note("registerInputHandlers at rva {:X}", gInputFn - gameBase());
    }
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
