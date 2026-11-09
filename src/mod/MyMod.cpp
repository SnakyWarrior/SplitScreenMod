#include "mod/MyMod.h"

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
    bool                    force = false;
    std::set<uintptr_t>     only;
    std::set<uintptr_t>     seen;
    ll::memory::FuncPtr     origin = nullptr;
};
static std::map<std::string, HookCfg> gCfg;

static bool decide(char const* name, void* ret, bool original) {
    static std::mutex m;
    std::lock_guard   lock(m);
    auto& c   = gCfg[name];
    auto  rva = reinterpret_cast<uintptr_t>(ret) - gameBase();
    bool  res = c.force && (c.only.empty() || c.only.contains(rva)) ? true : original;
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
static bool hookSlot(void* object, F method, ll::memory::FuncPtr detour, char const* name) {
    struct Raw {
        uint8_t const* code;
        int32_t        adjust; // only present in 16-byte member pointers
    } raw{};
    std::memcpy(&raw, &method, sizeof(method));
    auto index = thunkSlot(raw.code);
    if (!index) {
        note("ERROR: could not find the vtable slot for {} (thunk bytes {:02X})", name, fmt::join(raw.code, raw.code + 16, " "));
        return false;
    }
    auto self = static_cast<char*>(object) + (sizeof(method) > 8 ? raw.adjust : 0);
    auto slot = *reinterpret_cast<ll::memory::FuncPtr**>(self) + *index;
    Hooked h{slot, *slot};
    gCfg[name].origin = h.original; // detours read this, so set it before the slot goes live
    ll::memory::modify(slot, sizeof(*slot), [&] { *slot = detour; });
    gHooks.push_back(h);
    note("Hooked {} (vtable slot {}, original rva {:X})", name, *index, reinterpret_cast<uintptr_t>(h.original) - gameBase());
    return true;
}

// splitscreen.cfg lets tests switch hooks and resolve crash addresses without rebuilding:
//   hook <name> trace          log every call site, keep the game's answer
//   hook <name> force [rva..]  return true (only at those call sites, if given)
//   lookup <hex rva>   log the game function at that offset from Minecraft.Windows.exe
static void readConfig(std::set<std::string>& hooks) {
    std::ifstream in(MyMod::getInstance().getSelf().getModDir() / "splitscreen.cfg");
    if (!in) {
        // Default: allow splitscreen only where MinecraftGame::isSplitscreenJoinEnabled asks for it. Forcing every
        // caller breaks other PC code paths.
        // shortcut: call-site RVA is for game 1.26.60 only, re-find it with "trace" when the game updates.
        hooks = {"allowSplitScreen"};
        gCfg["allowSplitScreen"].force = true;
        gCfg["allowSplitScreen"].only  = {0x876204};
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
            gCfg[arg].force = mode == "force";
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
    static int nextController = 1;
    auto       window         = GetForegroundWindow();
    DWORD      pid            = 0;
    GetWindowThreadProcessId(window, &pid);
    if ((GetAsyncKeyState(VK_F8) & 1) && pid == GetCurrentProcessId() && nextController <= 3) {
        runTrigger("join", nextController++);
    }
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
    ll::thread::ClientThreadExecutor::getDefault().executeAfter(pollTrigger, std::chrono::milliseconds(500));
}

MyMod& MyMod::getInstance() {
    static MyMod instance;
    return instance;
}

static std::set<std::string> gWanted;
static bool                  gPlatformHooked = false;
static auto fn(auto f) { return reinterpret_cast<ll::memory::FuncPtr>(f); }

// Gamepads are sorted into players when they connect, which happens before enable(), so hook as early as possible.
static void hookPlatform() {
    static std::mutex m;
    std::lock_guard   lock(m);
    auto platform = ll::service::getAppPlatform();
    if (gPlatformHooked || !platform || !gWanted.contains("allowSplitScreen")) return;
    gPlatformHooked =
        hookSlot(platform.as_ptr(), &AppPlatform::allowSplitScreen, fn(&allowSplitScreenDetour), "allowSplitScreen");
}

bool MyMod::load() {
    note("SplitScreenMod loading");
    readConfig(gWanted);
    // AppPlatform does not exist yet at load; catch it as soon as it does, before gamepads are enumerated.
    std::thread([] {
        for (int i = 0; i < 60000 && !gPlatformHooked; ++i) {
            if (ll::service::getAppPlatform()) {
                hookPlatform();
                note("load: platform hooked after {} ms", i);
                return;
            }
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
        note("ERROR: game not ready (platform hooked: {}, client: {})", gPlatformHooked, bool(client));
        return false;
    }
    IMinecraftGame& game = client->getMinecraftGame();
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
