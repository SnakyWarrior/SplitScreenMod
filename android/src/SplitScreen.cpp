// SplitScreenMod for Android: first, experimental version.
//
// Minecraft Bedrock has split screen built in (it is used on consoles); each platform's AppPlatform decides with
// allowSplitScreen() whether it is offered. The Windows mod turns that on, then fixes up controller handling. On
// Android nothing about the game's code is known yet, so this version:
//   1. finds the game's classes by their RTTI names and writes their vtables to splitscreen.log;
//   2. turns allowSplitScreen on when it can be identified for certain (by its exported symbol);
//   3. otherwise only lists the likely candidates, so the next version can pick the right one.

#include "SplitScreen.h"

#include "Elf.h"

#include <pl/memory/Hook.hpp>
#include "InputProbe.h"
#include "DeviceManagerProbe.h"
#include "ControllerSplit.h"
#include "VTrace.h"
#include "PostTrace.h"
#include "FormProbe.h"
#include "Menus.h"


#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

namespace splitscreen {

namespace {

constexpr char const* kGameLibrary = "libminecraftpe.so";

std::mutex    gLogMutex;
std::ofstream gLog;
std::ofstream gDownloadLog; // the same lines in Download/SplitScreenMod-android.log, where it is easy to find

template <class... Args>
void note(fmt::format_string<Args...> f, Args&&... args) {
    auto line = fmt::format(f, std::forward<Args>(args)...);
    SplitScreenMod::instance().getSelf().getLogger().info("{}", line);
    std::lock_guard lock(gLogMutex);
    if (gLog) gLog << line << std::endl;
    if (gDownloadLog) gDownloadLog << line << std::endl;
}

// Download/SplitScreenMod-android.log gets every line as it is written (a copy made once would miss what happens
// later in the game). Android lets an app create files in Download by path; if it does not, the log stays in the
// mod folder only.
void openDownloadLog() {
    for (char const* dir : {"/storage/emulated/0/Download", "/sdcard/Download"}) {
        auto path = std::filesystem::path(dir) / "SplitScreenMod-android.log";
        std::lock_guard lock(gLogMutex);
        gDownloadLog.open(path, std::ios::trunc);
        if (gDownloadLog) return;
        gDownloadLog.clear();
    }
}

// config/config.json: { "forceSplitScreen": true }
bool configFlag(std::string_view key, bool fallback) {
    std::ifstream in(SplitScreenMod::instance().getSelf().getConfigDir() / "config.json");
    if (!in) return fallback;
    std::stringstream ss;
    ss << in.rdbuf();
    auto text = ss.str();
    auto at   = text.find(fmt::format("\"{}\"", key));
    if (at == std::string::npos) return fallback;
    at = text.find(':', at);
    if (at == std::string::npos) return fallback;
    auto rest = text.find_first_not_of(" \t\r\n", at + 1);
    if (rest == std::string::npos) return fallback;
    if (text.compare(rest, 4, "true") == 0) return true;
    if (text.compare(rest, 5, "false") == 0) return false;
    return fallback;
}

struct ClassInfo {
    std::string             name;
    uintptr_t               typeInfo = 0;
    std::vector<uintptr_t*> vtables;
};

std::vector<ClassInfo> findClasses(Module const& m, std::string_view needle) {
    std::vector<ClassInfo> out;
    for (auto const& t : findTypeNames(m, needle)) {
        ClassInfo c;
        c.name     = t.name;
        c.typeInfo = findTypeInfo(m, t.address);
        if (c.typeInfo) c.vtables = findVtables(m, c.typeInfo);
        out.push_back(std::move(c));
    }
    return out;
}

// Exported symbols that would identify the functions for certain, if the game exports them.
struct Known {
    char const* label;
    char const* symbol;
};
constexpr Known kKnown[] = {
    {"AppPlatform::allowSplitScreen", "_ZN11AppPlatform16allowSplitScreenEv"},
    {"AppPlatform_android::allowSplitScreen", "_ZN19AppPlatform_android16allowSplitScreenEv"},
    {"MinecraftGame::isSplitscreenJoinEnabled", "_ZNK13MinecraftGame24isSplitscreenJoinEnabledEv"},
    {"MinecraftGame::canAddSplitscreenSubClient", "_ZNK13MinecraftGame26canAddSplitscreenSubClientEi"},
    {"MinecraftGame::addSubClientInstanceAndAssociateToUser", "_ZN13MinecraftGame38addSubClientInstanceAndAssociateToUserEi"},
};

// Where allowSplitScreen is in AppPlatform's vtable, found from the order of AppPlatform's virtual functions
// (the same on every platform; LeviLamina's AppPlatform.h lists them):
//   swapBuffers() = 0, discardBackbuffer(), getSystemLocale() = 0, getTermsOfUseHyperlink(), pickImage() = 0, ...
//   allowSplitScreen() 28 slots after swapBuffers, then splitScreenRenderDistScalar(uint numPlayers).
// The three pure virtual functions two slots apart mark the place; splitScreenRenderDistScalar, which starts with
// "fmov s0, #1.0", confirms it. In the 1.26 Android game these are slots 77, 79, 81 and 105.
constexpr size_t kAllowSplitScreenAfterSwapBuffers = 28;

std::optional<size_t> findAllowSplitScreenSlot(Module const& m, uintptr_t* vt, size_t count) {
    std::optional<size_t> found;
    for (size_t i = 0; i + kAllowSplitScreenAfterSwapBuffers + 1 < count; ++i) {
        if (!isPureVirtual(vt[i]) || !isPureVirtual(vt[i + 2]) || !isPureVirtual(vt[i + 4])) continue;
        if (isPureVirtual(vt[i + 1]) || isPureVirtual(vt[i + 3])) continue;
        size_t slot = i + kAllowSplitScreenAfterSwapBuffers;
        auto   next = reinterpret_cast<uint32_t const*>(vt[slot + 1]);
        if (!m.isCode(vt[slot + 1]) || next[0] != 0x1E2E1000) { // fmov s0, #1.0
            note("allowSplitScreen: pure virtual functions at slot {}, but slot {} is not splitScreenRenderDistScalar ({})",
                 i, slot + 1, describeFunction(m, vt[slot + 1]));
            continue;
        }
        if (found) {
            note("allowSplitScreen: more than one match (slots {} and {}), not changing anything", *found, slot);
            return std::nullopt;
        }
        found = slot;
    }
    return found;
}

using AllowSplitScreenFn = bool (*)(void*);
AllowSplitScreenFn gOriginalAllowSplitScreen = nullptr;

std::map<uintptr_t*, uintptr_t> gPatched; // slot -> original

std::atomic<uint64_t> gAllowSplitScreenCalls{0};

bool allowSplitScreenDetour(void* self) {
    ++gAllowSplitScreenCalls;
    static std::once_flag first;
    std::call_once(first, [self] {
        note("allowSplitScreen asked for the first time; the game alone would answer {}",
             gOriginalAllowSplitScreen ? (gOriginalAllowSplitScreen(self) ? "true" : "false") : "?");
    });
    return true;
}

// Every platform class (AppPlatform_android and its relatives) gets the slot changed, including the ones that
// override allowSplitScreen with their own "return false". Classes that only have "AppPlatform" in their name
// (AppPlatformListener, ...) have far fewer virtual functions than AppPlatform and are skipped.
void patchSlotEverywhere(Module const& m, std::vector<ClassInfo> const& classes, size_t baseCount, size_t slot,
                         uintptr_t original, char const* what) {
    for (auto const& c : classes)
        for (auto* vt : c.vtables) {
            if (countSlots(m, vt, 2000) < baseCount) continue;
            if (vt[slot] != original && !constantReturn(vt[slot])) {
                note("  {} slot {}: {} does more than return a constant, left alone", c.name, slot,
                     describeFunction(m, vt[slot]));
                continue;
            }
            uintptr_t old = vt[slot];
            if (writeSlot(&vt[slot], reinterpret_cast<uintptr_t>(&allowSplitScreenDetour))) {
                gPatched[&vt[slot]] = old;
                note("  {} slot {}: {} now returns true", c.name, slot, what);
            } else {
                note("  {} slot {}: ERROR: could not write the vtable", c.name, slot);
            }
        }
}

// --- joining: MinecraftGame's splitscreen checks ---------------------------------------------------------------
// In LeviLamina's IMinecraftGame.h, isSplitscreenJoinEnabled() and canAddSplitscreenSubClient(int) are virtual
// functions 172 and 173 and addSubClientInstanceAndAssociateToUser(int) is 210. In the Android game the last one is
// recognisable: "mov w2, #1; b _addSubClientInstanceAndAssociateToUserInternal" (it passes true). It is the anchor:
// the two checks are 38 and 37 slots before it.
constexpr size_t kAddSubClientSlot = 210, kJoinEnabledBack = 38, kCanAddBack = 37;

Module const* gGameModule = nullptr;

// Controllers 1-3 that play as an added player. Start on them is just Start (the pause menu): adding a player again
// for the same controller made the game release sub client 0 and crash (0.3.1).
std::mutex    gPlayingMutex;
std::set<int> gPlaying;
// players asked to leave: no longer in game::players(), so no menu opens on a screen that is about to go (opening
// one there crashed the game)
std::set<int> gLeaving;
std::map<int, std::pair<int, int>> gHandOvers; // leaving player -> (its controller, where that goes) once it is gone

void forgetPlayers(char const* why, int sub) {
    std::optional<std::pair<int, int>> handOver;
    {
        std::lock_guard lock(gPlayingMutex);
        gLeaving.erase(sub);
        if (!gPlaying.erase(sub)) return;
        if (auto it = gHandOvers.find(sub); it != gHandOvers.end()) {
            handOver = it->second;
            gHandOvers.erase(it);
        }
    }
    note("join: {}({}): player {} is gone", why, sub, sub + 1);
    menus::onPlayerLeft(sub);
    if (handOver)
        runOnInputThread([h = *handOver] {
            blockController(h.first, false);
            swapControllers(h.first, h.second);
        }, std::chrono::seconds(1));
    runOnInputThread([sub] { blockController(sub, false); }, std::chrono::seconds(1)); // once it is gone for good
}

using SubIdFn = void (*)(void*, int);
SubIdFn gQueueRemoval = nullptr, gReleaseSubId = nullptr;
void queueRemovalDetour(void* self, int sub) {
    uintptr_t ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    note("join: queueSubclientRemoval({}) from +0x{:X}", sub, gGameModule ? ret - gGameModule->base : ret);
    gQueueRemoval(self, sub);
    forgetPlayers("queueSubclientRemoval", sub);
}
void releaseSubIdDetour(void* self, int sub) {
    uintptr_t ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    note("join: releaseClientSubId({}) from +0x{:X}", sub, gGameModule ? ret - gGameModule->base : ret);
    gReleaseSubId(self, sub);
    forgetPlayers("releaseClientSubId", sub);
}

using JoinEnabledFn = bool (*)(void*);
using CanAddFn      = bool (*)(void*, int);
using AddSubFn      = void (*)(void*, int);
JoinEnabledFn          gJoinEnabled = nullptr;
CanAddFn               gCanAdd      = nullptr;
AddSubFn               gAddSub      = nullptr;
std::atomic<void*>     gMinecraftGame{nullptr};
std::atomic<int>       gJoinLogged{0}, gCanAddLogged{0};
std::atomic<int>       gLastJoin{-1};

bool joinEnabledDetour(void* self) {
    gMinecraftGame = self;
    bool game      = gJoinEnabled(self);
    if (gLastJoin.exchange(game) != int(game) || gJoinLogged < 3) {
        ++gJoinLogged;
        note("join: isSplitscreenJoinEnabled asked; the game alone would answer {}", game);
    }
    return true;
}

thread_local int tGameCanAdd = -1; // the game's own canAddSplitscreenSubClient answer while the mod adds a player

bool canAddDetour(void* self, int controller) {
    gMinecraftGame = self;
    bool game      = gCanAdd(self, controller);
    tGameCanAdd    = game;
    bool answer    = controller >= 1 && controller <= 3 ? true : game;
    if (gCanAddLogged < 40) {
        ++gCanAddLogged;
        note("join: canAddSplitscreenSubClient({}) asked; the game alone would answer {}, answering {}", controller, game,
             answer);
    }
    return answer;
}

// The game's own join prompt ("press Start to join") calls addSubClientInstanceAndAssociateToUser; as on Windows
// (joinRequestDetour) the menus decide who joins, and the mod's own call (tOurJoin) goes through.
thread_local bool tOurJoin = false;
void addSubDetour(void* self, int controller) {
    uintptr_t ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    if (!tOurJoin) {
        note("join: the game asks to add a player for controller {} (from +0x{:X})", controller,
             gGameModule ? ret - gGameModule->base : ret);
        if (controller <= 0) return gAddSub(self, controller);
        return menus::onJoinRequest(controller);
    }
    note("join: addSubClientInstanceAndAssociateToUser({}) called", controller);
    gAddSub(self, controller);
    note("join: addSubClientInstanceAndAssociateToUser({}) returned", controller);
}


// Lending player 1's user (as the Windows mod does). The user manager keeps a vector of shared_ptr<User>; virtual 5
// returns the primary user and virtual 53 the user of a controller (empty when there is none), both through x8.
// For a controller the mod added as player 2-4, virtual 53 returns the primary user and virtual 6 ("has a user")
// true, so the new player gets a user instead of none.
struct UserPtr { // a shared_ptr<User>: returned through x8 like the game's (non-trivial type), moved bitwise
    void* object  = nullptr;
    void* control = nullptr;
    UserPtr() = default;
    UserPtr(UserPtr const& other) : object(other.object), control(other.control) {}
    ~UserPtr() {}
};
using UserByControllerFn = UserPtr (*)(void*, int);
using PrimaryUserFn      = UserPtr (*)(void*);
UserByControllerFn gUserByController = nullptr;
PrimaryUserFn      gPrimaryUser      = nullptr;
std::mutex         gLendMutex;
std::set<int>      gLending; // controllers that borrow player 1's user
std::atomic<int>   gLendLogged{0};

bool lends(int controller) {
    if (controller < 1 || controller > 3) return false;
    std::lock_guard lock(gLendMutex);
    return gLending.contains(controller);
}

// Only when the game finds no user for a lent controller (0.3.0): the user the game makes for the new player works.
UserPtr userByControllerDetour(void* self, int controller) {
    UserPtr user = gUserByController(self, controller);
    if (user.object || !lends(controller)) return user;
    if (gLendLogged < 10) {
        ++gLendLogged;
        note("join: controller {} has no user: lending player 1's", controller);
    }
    return gPrimaryUser(self);
}

// user manager virtuals 6, 39 (bool(int)) and 7 (void(int)), logged
using UserBoolFn = bool (*)(void*, int);
using UserVoidFn = void (*)(void*, int);
UserBoolFn gUser6 = nullptr, gUser39 = nullptr;
UserVoidFn gUser7 = nullptr;
bool user6Detour(void* self, int c) {
    bool r = gUser6(self, c) || lends(c);
    note("join: user manager virtual 6({}) = {}", c, r);
    return r;
}
thread_local int tControllerTaken = -1; // user manager virtual 39's answer while the mod adds a player

bool user39Detour(void* self, int c) {
    bool r = gUser39(self, c);
    tControllerTaken = r;
    note("join: user manager virtual 39({}) = {}", c, r);
    return r;
}
void user7Detour(void* self, int c) {
    uintptr_t ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    note("join: user manager virtual 7({}) from +0x{:X}", c, gGameModule ? ret - gGameModule->base : ret);
    gUser7(self, c);
}

// _addSubClientInstanceAndAssociateToUserInternal(game, controller, true) asks the user manager (MinecraftGame
// virtual 53, returned as a NonOwnerPointer {control block, ref count, object}) with its virtual 28
// (controller, true). If that answers true it starts an asynchronous user sign-in for the controller and waits for
// a callback (which never came on Android); if false it creates the new player at once. While the mod adds a
// player, virtual 28 answers false for controllers 1-3.
using UserCheckFn = bool (*)(void*, int, bool);
UserCheckFn      gUserCheck = nullptr;
thread_local bool tAddingPlayer = false;
constexpr size_t kUserManagerSlot = 53, kUserCheckSlot = 28;

bool userCheckDetour(void* self, int controller, bool flag) {
    bool game = gUserCheck(self, controller, flag);
    bool answer = tAddingPlayer && controller >= 1 && controller <= 3 ? false : game;
    note("join: user manager check({}, {}): the game alone would answer {}, answering {}", controller, flag, game, answer);
    return answer;
}

void hookUserManager(Module const& module, void* game) {
    if (gUserCheck) return;
    struct NonOwner {
        uint64_t control, refs, object;
    };
    auto getUserManager = reinterpret_cast<NonOwner (*)(void*)>((*reinterpret_cast<uintptr_t**>(game))[kUserManagerSlot]);
    NonOwner users      = getUserManager(game); // keeps one reference: never released, it lives as long as the game
    if (!users.object) return note("join: ERROR: no user manager");
    auto* vt = *reinterpret_cast<uintptr_t**>(users.object);
    note("join: user manager {} ({}), check {}", className(module, users.object), countSlots(module, vt, 400),
         describeFunction(module, vt[kUserCheckSlot]));
    if (countSlots(module, vt, 400) <= kUserCheckSlot) return note("join: ERROR: the user manager has too few functions");
    gPrimaryUser      = reinterpret_cast<PrimaryUserFn>(vt[5]);
    gUserByController = reinterpret_cast<UserByControllerFn>(vt[53]);
    writeSlot(&vt[53], reinterpret_cast<uintptr_t>(&userByControllerDetour));
    gUser6  = reinterpret_cast<UserBoolFn>(vt[6]);
    gUser39 = reinterpret_cast<UserBoolFn>(vt[39]);
    gUser7  = reinterpret_cast<UserVoidFn>(vt[7]);
    writeSlot(&vt[6], reinterpret_cast<uintptr_t>(&user6Detour));
    writeSlot(&vt[39], reinterpret_cast<uintptr_t>(&user39Detour));
    writeSlot(&vt[7], reinterpret_cast<uintptr_t>(&user7Detour));
    gUserCheck = reinterpret_cast<UserCheckFn>(vt[kUserCheckSlot]);
    if (!writeSlot(&vt[kUserCheckSlot], reinterpret_cast<uintptr_t>(&userCheckDetour))) {
        gUserCheck = nullptr;
        note("join: ERROR: could not write the user manager vtable");
    }
    // the user manager functions the game called while adding a player (0.2.8): their code and their results
    static std::vector<size_t> const kAsked = {6, 8, 38, 53, 54, 70};
    watchResults(module, vt, kAsked, "UserManager", [](std::string const& l) { note("{}", l); });
    // after the patches above; MinecraftGame uses trampolines 0-319
    traceVtable(module, vt, countSlots(module, vt, 400), 320, "UserManager", [](std::string const& l) { note("{}", l); });
}

// The check before adding a player (+0x9CED20C in 1.26), "W"(game, controller, uint8_t* out, bool):
//   canAddSplitscreenSubClient(controller); game+512 != 0; not (primary client virtual 113 and a
//   GameControllerManager check); a GameControllerManager check(controller); user manager virtual 6(controller);
//   not user manager virtual 39(controller) and (game+496)[25] <= 4; then user manager virtual 7(controller).
// While the mod adds a player for controller 1-3 and W refuses, it answers true, so the game creates the player.
using AddCheckFn = bool (*)(void*, int, uint8_t*, bool);
AddCheckFn gAddCheck = nullptr;

bool addCheckDetour(void* game, int controller, uint8_t* out, bool flag) {
    bool result = gAddCheck(game, controller, out, flag);
    bool answer = result;
    if (!result && tAddingPlayer && controller >= 1 && controller <= 3 && tGameCanAdd == 1 && tControllerTaken != 1)
        answer = true;
    if (answer && tAddingPlayer) {
        std::lock_guard lock(gPlayingMutex);
        gPlaying.insert(controller);
    }
    note("join: may a player be added for controller {}? the game alone would answer {}, answering {}", controller,
         result, answer);
    return answer;
}


// A controller other than the first pressed Start: add a player for it, once (the game's own "press Start to join"
// does not react to it on Android).
std::mutex gJoinMutex;
std::map<int, std::chrono::steady_clock::time_point> gJoinTimes;

void joinController(int controller) {
    void* game = gMinecraftGame.load();
    {
        std::lock_guard lock(gJoinMutex);
        auto now = std::chrono::steady_clock::now();
        if (auto it = gJoinTimes.find(controller); it != gJoinTimes.end() && now - it->second < std::chrono::seconds(3))
            return;
        if (!game || !gAddSub) return note("join: controller {} pressed Start, but MinecraftGame is not known yet", controller);
        {
            std::lock_guard playing(gPlayingMutex);
            if (gPlaying.contains(controller)) return; // it already plays: Start opens its pause menu
        }
        gJoinTimes[controller] = now;
    }
    note("join: adding player {} for controller {}", controller + 1, controller);
    if (gGameModule) hookUserManager(*gGameModule, game);
    {
        auto const* g     = reinterpret_cast<uint8_t const*>(game);
        uintptr_t   at496 = *reinterpret_cast<uintptr_t const*>(g + 496);
        note("join: MinecraftGame+512 = 0x{:X}, (MinecraftGame+496)[25] = {}", *reinterpret_cast<uintptr_t const*>(g + 512),
             at496 ? int(reinterpret_cast<uint8_t const*>(at496)[25]) : -1);
    }
    startTracing(60);
    logResults(60);
    {
        std::lock_guard lock(gLendMutex);
        gLending.insert(controller);
    }
    tAddingPlayer    = true;
    tGameCanAdd      = -1;
    tControllerTaken = -1;
    tOurJoin         = true;
    addSubDetour(game, controller);
    tOurJoin      = false;
    tAddingPlayer = false;
}

// ClientInstance virtual 24 runs every frame for each client (args true, true). When the client's leave flags are
// set and the client is not the primary one, it closes the client and calls MinecraftGame::queueSubclientRemoval,
// the game's own way a player leaves (calling queueSubclientRemoval alone left the client half there, and the game
// crashed on a destroyed mutex). The leave flags +0x6E0..+0x6E5 are logged when they change, to see what the game
// sets when a player leaves from the pause menu. The client of each sub client comes from getClientSubId (slot 303).
using TickFn     = void (*)(void*, bool, bool);
using GetSubIdFn = uint64_t (*)(void*);
constexpr size_t   kLeaveFlags  = 0x6E0;
TickFn             gClientTick  = nullptr;
uintptr_t*         gClientVt    = nullptr;
using RequestLeaveFn = void (*)(void*, bool, bool);
RequestLeaveFn     gRequestLeave = nullptr; // ClientInstance virtual 15
GetSubIdFn         gGetSubId    = nullptr;
std::atomic<void*> gClientBySub[4] = {};
std::atomic<bool>  gLeaveWanted[4] = {};
uint64_t           gFlagsSeen[4]   = {};

uint64_t getSubIdDetour(void* self) {
    uint64_t sub = gGetSubId(self);
    if ((sub & 0xFF) < 4 && gClientBySub[sub & 0xFF].load(std::memory_order_relaxed) != self) {
        gClientBySub[sub & 0xFF].store(self, std::memory_order_relaxed);
        note("leave: client of sub client {} is {}", sub & 0xFF, self);
    }
    return sub;
}

uint64_t leaveFlags(void* client) {
    uint64_t flags = 0;
    std::memcpy(&flags, static_cast<uint8_t*>(client) + kLeaveFlags, 6);
    return flags;
}

void clientTickDetour(void* self, bool a, bool b) {
    int sub = gGetSubId ? int(gGetSubId(self) & 0xFF) : -1;
    if (sub >= 1 && sub < 4 && gLeaveWanted[sub].exchange(false)) {
        // What the pause menu's Save & Leave calls (0.9.7 log: ClientInstance virtual 15 with true, true, right before
        // the game sets the leave bytes; the Windows version's requestLeaveGameAsync). Only setting the bytes
        // (+0x6E0, +0x6E1, +0x6E4) left the player's actor in the game's lists after it was removed, and the game
        // crashed ("terminating") on it.
        if (gRequestLeave) {
            note("leave: player {} leaves: requestLeaveGame on client {}", sub + 1, self);
            gRequestLeave(self, true, true);
        } else {
            note("leave: player {} leaves: client {} bytes +0x6E0, +0x6E1, +0x6E4 = 1", sub + 1, self);
            auto* bytes = static_cast<uint8_t*>(self);
            bytes[0x6E0] = bytes[0x6E1] = bytes[0x6E4] = 1;
        }
    }
    if (sub >= 0 && sub < 4) {
        uint64_t flags = leaveFlags(self);
        if (flags != gFlagsSeen[sub]) {
            note("leave: client of sub client {} flags +0x6E0: {:012X} -> {:012X} (args {} {})", sub, gFlagsSeen[sub],
                 flags, a, b);
            gFlagsSeen[sub] = flags;
        }
    }
    gClientTick(self, a, b);
    if (sub >= 0 && sub < 4) {
        uint64_t flags = leaveFlags(self);
        if (flags != gFlagsSeen[sub]) {
            note("leave: client of sub client {} flags +0x6E0 after its frame: {:012X} -> {:012X}", sub,
                 gFlagsSeen[sub], flags);
            gFlagsSeen[sub] = flags;
        }
    }
}

void leaveSplitScreen(int sub) {
    void* client = sub >= 1 && sub < 4 ? gClientBySub[sub].load() : nullptr;
    if (!gClientTick || !client)
        return note("menus: ERROR: no client for player {} (tick {})", sub + 1, gClientTick != nullptr);
    note("menus: player {} leaves (client {}) at its next frame", sub + 1, client);
    gLeaveWanted[sub] = true;
}

} // namespace

void logLine(std::string const& line) { note("{}", line); }

// The player leaves once its last menu has closed (1.5 s after it was answered).
void leaveWhenQuiet(int sub) {
    runOnInputThread([sub] {
        if (msSinceMenu(sub) < 1500) return leaveWhenQuiet(sub);
        leaveSplitScreen(sub);
    }, std::chrono::milliseconds(500));
}

// What the menus (Menus.cpp) ask of the game.
namespace game {

std::map<int, int> players() {
    std::map<int, int> out{{0, 0}};
    std::lock_guard    lock(gPlayingMutex);
    for (int sub : gPlaying)
        if (!gLeaving.contains(sub)) out[sub] = sub;
    return out;
}

bool hasController(int controller) { return controllerConnected(controller); }

void requestLeave(int sub) {
    if (sub < 1 || sub > 3) return; // player 1 leaving is leaving the world
    {
        std::lock_guard lock(gPlayingMutex);
        if (!gPlaying.contains(sub) || !gLeaving.insert(sub).second) return;
    }
    cancelMenu(sub);
    // A leaving player gets no new presses (their releases still come).
    blockController(sub, true);
    // Removed at once, a player whose menu is still closing takes the game down with it (a destroyed mutex): the
    // player leaves once its screen has closed.
    leaveWhenQuiet(sub);
}

void moveController(int from, int to) { swapControllers(from, to); }

void handOver(int sub, int from, int to) {
    {
        std::lock_guard lock(gPlayingMutex);
        gHandOvers[sub] = {from, to};
    }
    note("menus: player {} leaves, then controller {} plays as controller {}", sub + 1, from, to);
    requestLeave(sub);
}

void freeFirstController() {
    auto playing = players();
    for (int c = 1; c <= 3; ++c)
        if (!playing.contains(c) && !controllerConnected(c)) {
            note("menus: controller taken from player 1 (it waits for Start as controller {})", c);
            return swapControllers(0, c);
        }
    note("menus: no free place for player 1's controller");
}

void join(int controller, std::function<void(int)> joined) {
    runOnInputThread([controller, joined = std::move(joined)] {
        // the new player's sub client id is the first free one: the controller takes that number first
        auto playing = players();
        int  sub     = 1;
        while (sub <= 3 && playing.contains(sub)) ++sub;
        if (sub > 3) {
            note("join: no free place for controller {}", controller);
            if (joined) joined(-1);
            return;
        }
        if (controller != sub) swapControllers(controller, sub);
        joinController(sub);
        if (joined) joined(players().contains(sub) ? sub : -1);
    });
}

} // namespace game

namespace {

bool hookClientLeave(Module const& game) {
    constexpr uint32_t kLdrSubIdSlot = 0xF944BD08; // ldr x8, [x8, #0x978]: getClientSubId, slot 303
    constexpr uint32_t kLdrRemoval   = 0xF9402908; // ldr x8, [x8, #0x50]: MinecraftGame::queueSubclientRemoval
    for (auto const& c : findClasses(game, "14ClientInstance")) {
        if (c.name != "14ClientInstance") continue;
        for (uintptr_t* vt : c.vtables) {
            size_t count = countSlots(game, vt, 2000);
            note("leave: ClientInstance vtable +0x{:X}, {} slots", reinterpret_cast<uintptr_t>(vt) - game.base, count);
            if (count <= 303) continue;
            for (size_t i = 0; i < count; ++i) {
                if (!game.isCode(vt[i]) || !game.isCode(vt[i] + 0x400)) continue;
                auto const* code = reinterpret_cast<uint32_t const*>(vt[i]);
                for (size_t k = 0; k < 0x100 - 8; ++k) {
                    if (code[k] != kLdrSubIdSlot) continue;
                    bool removal = false;
                    for (size_t j = k + 1; j < k + 8; ++j) removal |= code[j] == kLdrRemoval;
                    if (!removal) continue;
                    note("leave: the client frame is slot {} (+0x{:X}), getClientSubId slot 303 ({})", i,
                         vt[i] - game.base, describeFunction(game, vt[303]));
                    gClientTick = reinterpret_cast<TickFn>(vt[i]);
                    gClientVt   = vt;
                    // requestLeaveGame: the slot the pause menu calls (1.26: 15), checked to be a function
                    if (count > 15 && game.isCode(vt[15])) gRequestLeave = reinterpret_cast<RequestLeaveFn>(vt[15]);
                    note("leave: requestLeaveGame is slot 15 ({})", describeFunction(game, vt[15]));
                    gGetSubId   = reinterpret_cast<GetSubIdFn>(vt[303]);
                    // The game's own "this player leaves" (the pause menu's Save & Leave): a function that sets the
                    // "leave requested" byte +0x6E1 (strb wN, [xM, #0x6E1], before its first ret). The mod only sets
                    // the bytes; the game's function may do more first, and players removed by the mod's bytes made the
                    // game crash on a removed player's actor. Each candidate is traced (which one the pause menu calls,
                    // and with what).
                    constexpr uint32_t kStrbRequested = 0x39000000 | (0x6E1 << 10);
                    size_t             traced         = 0;
                    for (size_t w = 0; w < count; ++w) {
                        if (!game.isCode(vt[w]) || !game.isCode(vt[w] + 0x800)) continue;
                        auto const* fn = reinterpret_cast<uint32_t const*>(vt[w]);
                        for (size_t n = 0; n < 0x200 && fn[n] != 0xD65F03C0; ++n)
                            if ((fn[n] & 0xFFFFFC00) == kStrbRequested && (fn[n] & 31) != 31) {
                                note("leave: slot {} (+0x{:X}) sets +0x6E1 at +0x{:X}", w, vt[w] - game.base, n * 4);
                                if (traced < 8) {
                                    static std::string names[8];
                                    names[traced] = fmt::format("ClientInstance{}", w);
                                    traceVtable(game, vt + w, 1, 412 + traced, names[traced].c_str(),
                                                [](std::string const& l) { note("{}", l); }, 20);
                                    ++traced;
                                }
                                break;
                            }
                    }
                    return writeSlot(&vt[i], reinterpret_cast<uintptr_t>(&clientTickDetour))
                        && writeSlot(&vt[303], reinterpret_cast<uintptr_t>(&getSubIdDetour));
                }
            }
        }
    }
    note("leave: ERROR: ClientInstance::requestLeaveGame not found");
    return false;
}

// The Split screen menu, on Select (View / Back) of a controller that plays. The added player's sub client id is its
// controller number (1-3).
using PauseEventFn = uint64_t (*)(void*, void*);
PauseEventFn     gPauseEvent = nullptr;
std::atomic<int> gPauseEventsLogged{0};

// The resource pack's Split screen button in the pause menu sends "button.splitscreen_menu". The game names buttons
// by the FNV-1a hash of their name (0.9.11 log: B8B6D7EF for it, D65033DE for button.menu_continue); a button event
// is type 1, then (at +8) the hash, then (at +16) the state (1 = pressed). As on Windows, the pause menu closes (16: what Resume
// answers) and the player who pressed it gets the Split screen menu.
constexpr uint32_t fnv1a(std::string_view s) {
    uint32_t h = 0x811C9DC5;
    for (char c : s) h = (h ^ uint8_t(c)) * 0x01000193;
    return h;
}
constexpr uint32_t kSplitScreenButton = fnv1a("button.splitscreen_menu");
constexpr uint32_t kTouchPress        = fnv1a("button.menu_select");
constexpr uint64_t kCloseScreen       = 16;

uint64_t pauseEventDetour(void* self, void* event) {
    uint32_t words[8] = {};
    std::memcpy(words, event, sizeof words);
    if (words[0] == 1 && words[2] == kSplitScreenButton) { // type, (padding), hash, (?), state
        if ((words[4] & 0xFF) != 1) return 0;
        // what pressed it (at +12): button.menu_select is a touch (player 1's), button.menu_ok a controller's A (the
        // controller that pressed a button last)
        int who = words[3] == kTouchPress ? 0 : lastPresser();
        note("pause menu: Split screen pressed by player {}", who + 1);
        runOnInputThread([who] { menus::onSettingsButton(who); }, std::chrono::milliseconds(300));
        return kCloseScreen;
    }
    uint64_t result = gPauseEvent(self, event);
    if (words[0] != 5 && words[0] != 8 && gPauseEventsLogged < 60) {
        ++gPauseEventsLogged;
        note("pause: event {} [{:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}] -> {} (screen {})", words[0], words[1],
             words[2], words[3], words[4], words[5], words[6], words[7], result, self);
    }
    return result;
}

void enableJoining(Module const& game) {
    static Module copy = game;
    gGameModule        = &copy;
    auto classes = findClasses(game, "13MinecraftGame");
    uintptr_t* vt = nullptr;
    for (auto const& c : classes)
        if (c.name == "13MinecraftGame" && !c.vtables.empty()) vt = c.vtables.front();
    if (!vt) return note("join: ERROR: the MinecraftGame vtable was not found");
    size_t count = countSlots(game, vt, 2000);
    // find the anchor near its expected slot
    size_t anchor = SIZE_MAX;
    for (size_t i = kAddSubClientSlot - 40; i < std::min(count, kAddSubClientSlot + 40); ++i) {
        auto const* code = reinterpret_cast<uint32_t const*>(vt[i]);
        if (game.isCode(vt[i]) && code[0] == 0x52800022 && (code[1] & 0xFC000000) == 0x14000000) {
            if (anchor != SIZE_MAX) return note("join: ERROR: two candidates for addSubClientInstanceAndAssociateToUser");
            anchor = i;
        }
    }
    if (anchor == SIZE_MAX) return note("join: ERROR: addSubClientInstanceAndAssociateToUser not found");
    size_t joinSlot = anchor - kJoinEnabledBack, canAddSlot = anchor - kCanAddBack;
    note("join: MinecraftGame vtable +0x{:X}, {} slots; addSubClient {} , isSplitscreenJoinEnabled {} ({}), "
         "canAddSplitscreenSubClient {} ({})",
         reinterpret_cast<uintptr_t>(vt) - game.base, count, anchor, joinSlot, describeFunction(game, vt[joinSlot]),
         canAddSlot, describeFunction(game, vt[canAddSlot]));
    gJoinEnabled = reinterpret_cast<JoinEnabledFn>(vt[joinSlot]);
    gCanAdd      = reinterpret_cast<CanAddFn>(vt[canAddSlot]);
    gAddSub      = reinterpret_cast<AddSubFn>(vt[anchor]);
    bool ok = writeSlot(&vt[joinSlot], reinterpret_cast<uintptr_t>(&joinEnabledDetour))
           && writeSlot(&vt[canAddSlot], reinterpret_cast<uintptr_t>(&canAddDetour))
           && writeSlot(&vt[anchor], reinterpret_cast<uintptr_t>(&addSubDetour));
    note("{}", ok ? "join: splitscreen joining turned on" : "join: ERROR: could not write the MinecraftGame vtable");
    // virtuals 10 and 16 (queueSubclientRemoval, releaseClientSubId: IMinecraftGame.h 9 and 15, one slot later here
    // like getUserManager 52 -> 53)
    gQueueRemoval = reinterpret_cast<SubIdFn>(vt[10]);
    gReleaseSubId = reinterpret_cast<SubIdFn>(vt[16]);
    writeSlot(&vt[10], reinterpret_cast<uintptr_t>(&queueRemovalDetour));
    hookClientLeave(game);
    // The code of the game's "terminating" crash when a player leaves (1.26: the same frames every time).
    for (auto [label, at] : {std::pair{"crash9", 0x88A4C30}, {"crash10", 0xFD8ECD0}, {"crash11", 0xFD8DD58},
                             {"crash12", 0xB7C1CC0}, {"crash13", 0xB803EEC}})
        dumpCode(game, label, game.base + at - 0x300, 0x340, [](std::string const& l) { note("{}", l); });
    writeSlot(&vt[16], reinterpret_cast<uintptr_t>(&releaseSubIdDetour));
    // On Windows the game's join prompt asks to add a player; on Android the game shows none, so Start on a controller
    // without a player asks (the same request; not outside a world). "startJoins": false leaves it to the game.
    if (ok) onStartPressed([startJoins = configFlag("startJoins", true)](int controller) {
        if (game::players().contains(controller)) return startTracing(60); // its pause menu (traced)
        note("join: controller {} pressed Start{}", controller, startJoins ? "" : " (the game's prompt decides)");
        if (startJoins) menus::onJoinRequest(controller);
    });
    if (ok) onSelectPressed(menus::onSettingsButton);
    if (ok) setPlayingPlayers([] {
        std::lock_guard  lock(gPlayingMutex);
        std::vector<int> out;
        for (int sub : gPlaying)
            if (!gLeaving.contains(sub)) out.push_back(sub); // a leaving player does not get a new controller
        return out;
    });
    if (ok) onLostPlayerTakenOver(menus::onControllerConnected);
    // W: in _addSubClientInstanceAndAssociateToUserInternal (the "b" target of slot 210), the first bl after
    // "add x2, sp, #164" (its out parameter)
    {
        auto const* code     = reinterpret_cast<uint32_t const*>(gAddSub);
        uintptr_t   internal = reinterpret_cast<uintptr_t>(gAddSub) + 4 + ((int64_t(code[1] & 0x3FFFFFF) << 38) >> 36);
        auto const* body     = reinterpret_cast<uint32_t const*>(internal);
        uintptr_t   check    = 0;
        for (int i = 0; i < 0x100 && !check; ++i)
            if (body[i] == 0x910293E2) // add x2, sp, #164
                for (int k = i + 1; k < i + 8; ++k)
                    if ((body[k] & 0xFC000000) == 0x94000000) {
                        check = internal + 4 * k + ((int64_t(body[k] & 0x3FFFFFF) << 38) >> 36);
                        break;
                    }
        if (!check || !game.isCode(check)) note("join: ERROR: the add-player check was not found");
        else if (pl::memory::hook(reinterpret_cast<pl::memory::FuncPtr>(check),
                                  reinterpret_cast<pl::memory::FuncPtr>(&addCheckDetour),
                                  reinterpret_cast<pl::memory::FuncPtr*>(&gAddCheck))
                 != 0)
            note("join: ERROR: could not hook the add-player check +0x{:X}", check - game.base);
        else note("join: add-player check +0x{:X} hooked", check - game.base);
    }
    // after the patches above, so the trace sees our detours' slots as the originals
    if (ok) traceVtable(game, vt, count, 0, "MinecraftGame", [](std::string const& l) { note("{}", l); });
    // The pause menu (its Split screen button) and the game's join prompt: which of their functions run, to find
    // how the Windows version's button and prompt work here.
    // PauseScreenController virtual 4 gets the screen's events (0.9.9 log: type 5 with two floats is the pointer
    // moving): every other event is logged, to find the Split screen button's press.
    for (auto const& c : findClasses(game, "21PauseScreenController"))
        if (c.name == "21PauseScreenController" && !c.vtables.empty() && countSlots(game, c.vtables.front(), 400) > 4) {
            gPauseEvent = reinterpret_cast<PauseEventFn>(c.vtables.front()[4]);
            writeSlot(&c.vtables.front()[4], reinterpret_cast<uintptr_t>(&pauseEventDetour));
            note("pause: watching the pause menu's events");
        }
    size_t listenerBase = 575;
    for (auto const& c : findClasses(game, "SplitscreenJoinListener"))
        for (auto* lvt : c.vtables) {
            size_t n = std::min<size_t>(countSlots(game, lvt, 400), 600 - listenerBase);
            if (!n) continue;
            note("join: tracing {} ({} slots)", c.name, n);
            traceVtable(game, lvt, n, listenerBase, "SplitscreenJoinListener", [](std::string const& l) { note("{}", l); });
            listenerBase += n;
        }
}

void probe() {
    using namespace std::chrono_literals;
    std::optional<Module> game;
    for (int i = 0; i < 600 && !(game = findModule(kGameLibrary)); ++i) std::this_thread::sleep_for(200ms);
    if (!game) return note("ERROR: {} was not loaded within 2 minutes", kGameLibrary);
    std::this_thread::sleep_for(1s); // let the loader finish relocating it
    note("game library {} at 0x{:X}", game->path, game->base);
    for (auto const& s : game->segments)
        note("  segment +0x{:X}..+0x{:X} {}{}{}", s.start - game->base, s.end - game->base, s.read ? 'r' : '-',
             s.write ? 'w' : '-', s.exec ? 'x' : '-');

    // 1. exported symbols
    void*     handle = dlopen(game->path.c_str(), RTLD_NOW | RTLD_NOLOAD);
    std::set<uintptr_t> allowSplitScreenFns;
    for (auto const& k : kKnown) {
        auto addr = handle ? reinterpret_cast<uintptr_t>(dlsym(handle, k.symbol)) : 0;
        note("symbol {}: {}", k.label, addr ? fmt::format("+0x{:X}", addr - game->base) : "not exported");
        if (addr && std::string_view(k.label).ends_with("::allowSplitScreen")) allowSplitScreenFns.insert(addr);
    }

    // How Java hands input to the game: the exported JNI functions about input, and how many exports there are.
    {
        auto exports = exportedFunctions(*game);
        note("{} exported functions", exports.size());
        size_t jni = 0;
        for (auto const& e : exports) {
            if (!e.name.starts_with("Java_")) continue;
            ++jni;
            std::string lower = e.name;
            for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            for (char const* word : {"key", "controller", "gamepad", "joystick", "input", "button", "motion", "axis",
                                     "device", "trigger", "stick", "touch"})
                if (lower.find(word) != std::string::npos) {
                    note("jni {} +0x{:X}", e.name, e.address - game->base);
                    break;
                }
        }
        note("{} JNI functions in all", jni);
    }
    // The split screen layout option: its name in the game's strings (for the layout menu).
    {
        int shown = 0;
        for (auto const& seg : game->segments) {
            if (!seg.read || seg.write) continue;
            auto const* begin = reinterpret_cast<char const*>(seg.start);
            size_t      size  = seg.end - seg.start;
            for (char const* needle : {"plitscreen", "plit_screen", "plitScreen", "plit-screen"}) {
                size_t n = strlen(needle);
                for (size_t at = 0; at + n <= size && shown < 80;) {
                    auto const* hit = static_cast<char const*>(memmem(begin + at, size - at, needle, n));
                    if (!hit) break;
                    char const* start = hit;
                    while (start > begin && start[-1] >= 0x20 && start[-1] < 0x7F && hit - start < 120) --start;
                    std::string text(start, strnlen(start, 160));
                    at = size_t(hit - begin) + std::max<size_t>(1, text.size() - size_t(hit - start));
                    if (text.find("irection") == std::string::npos && text.find("ayout") == std::string::npos
                        && text.find("ertical") == std::string::npos && text.find("orizontal") == std::string::npos
                        && text.find("option") == std::string::npos)
                        continue;
                    note("layout string +0x{:X}: {}", reinterpret_cast<uintptr_t>(start) - game->base, text);
                    ++shown;
                }
            }
        }
    }
    // The pause menu's buttons: their names ("button.menu_...") and the code that uses them (the pause menu sets up
    // a handler for each name, as the Windows version does for its Split screen button). In the background: it reads
    // all of the game's code.
    std::thread([game = *game] {
        std::vector<std::pair<uintptr_t, std::string>> names;
        for (auto const& seg : game.segments) {
            if (!seg.read || seg.write) continue;
            auto const* begin = reinterpret_cast<char const*>(seg.start);
            size_t      size  = seg.end - seg.start;
            for (size_t at = 0; at + 12 <= size && names.size() < 40;) {
                auto const* hit = static_cast<char const*>(memmem(begin + at, size - at, "button.menu_", 12));
                if (!hit) break;
                at = size_t(hit - begin) + 1;
                if (hit > begin && hit[-1] != '\0') continue;
                names.push_back({reinterpret_cast<uintptr_t>(hit), std::string(hit, strnlen(hit, 64))});
            }
        }
        for (auto& [addr, name] : names) note("pause: string +0x{:X} {}", addr - game.base, name);
        // code that loads one of them (adrp xN; add xN, xN, #lo12)
        std::map<uintptr_t, std::string> wanted;
        for (auto& [addr, name] : names)
            if (name == "button.menu_continue" || name == "button.menu_quit" || name == "button.menu_settings"
                || name == "button.menu_resume" || name == "button.menu_exit")
                wanted[addr] = name;
        int dumped = 0;
        for (auto const& seg : game.segments) {
            if (!seg.exec || wanted.empty()) continue;
            auto const* code = reinterpret_cast<uint32_t const*>(seg.start);
            size_t      n    = (seg.end - seg.start) / 4;
            for (size_t i = 0; i + 1 < n && dumped < 6; ++i) {
                uint32_t op = code[i];
                if ((op & 0x9F000000) != 0x90000000) continue; // adrp
                int       rd   = op & 31;
                uint32_t  next = code[i + 1];
                if ((next & 0xFFC00000) != 0x91000000 || int(next & 31) != rd || int((next >> 5) & 31) != rd) continue;
                uintptr_t pc   = seg.start + 4 * i;
                int64_t   imm  = (int64_t(((op >> 5) & 0x7FFFF) << 2 | ((op >> 29) & 3)) << 43) >> 31;
                uintptr_t addr = (pc & ~uintptr_t(0xFFF)) + imm + ((next >> 10) & 0xFFF);
                auto it = wanted.find(addr);
                if (it == wanted.end()) continue;
                note("pause: {} used at +0x{:X}", it->second, pc - game.base);
                dumpCode(game, fmt::format("pause{}", dumped).c_str(), pc - 0x100, 0x200,
                         [](std::string const& l) { note("{}", l); });
                ++dumped;
            }
        }
        note("pause: search done");
    }).detach();
    // which Android input functions the game imports (native input queue, Paddleboat controller library, ...)
    {
        auto imports = importedSymbols(*game);
        note("{} imported symbols", imports.size());
        for (auto const& name : imports)
            for (char const* prefix : {"AInput", "AKeyEvent", "AMotionEvent", "ALooper", "Paddleboat", "GameActivity",
                                       "ANativeActivity", "AGameController"})
                if (name.starts_with(prefix)) {
                    note("import {}", name);
                    break;
                }
        for (auto const& e : exportedFunctions(*game))
            if (e.name.find("addleboat") != std::string::npos || e.name.find("GameActivity") != std::string::npos
                || e.name.find("android_main") != std::string::npos || e.name.find("ANativeActivity") != std::string::npos)
                note("export {} +0x{:X}", e.name, e.address - game->base);
    }
    {
        auto log = [](std::string const& l) { note("{}", l); };
        for (auto const& e : exportedFunctions(*game)) {
            if (e.name == "GameActivityKeyEvent_fromJava") watchGameActivityKeys(*game, e.address, log);
        }

        // the controller manager the JellyBeanDeviceManager JNI functions call
        auto           calls   = findDeviceManagerCalls(*game, exportedFunctions(*game));
        uintptr_t      manager = 0;
        for (auto const& c : calls) {
            note("device manager: {} calls +0x{:X} on object +0x{:X} ({})", c.jni, c.method - game->base,
                 c.object - game->base, className(*game, c.object));
            manager = c.object;
        }
        if (calls.empty()) note("device manager: not found");
        else if (!configFlag("separateControllers", true)) note("separateControllers is off in config.json");
        else {
            uintptr_t added = 0, removed = 0;
            for (auto const& c : calls) {
                if (std::string_view(c.jni) == "onInputDeviceAddedNative") added = c.method;
                if (std::string_view(c.jni) == "onInputDeviceRemovedNative") removed = c.method;
            }
            setFirstControllerIsPlayer1(configFlag("firstControllerIsPlayer1", false));
            if (added) startControllerSplit(*game, manager, added, removed, log);
            else note("controllers: the manager's add function was not found");
        }
    }

    // 2. classes by RTTI. PlayerListPacket is a class every version has: if it is not found, the search itself failed.
    for (auto const& c : findClasses(*game, "16PlayerListPacket"))
        note("check: class {}: type_info {}, {} vtable(s)", c.name,
             c.typeInfo ? fmt::format("+0x{:X}", c.typeInfo - game->base) : "not found", c.vtables.size());
    auto platforms = findClasses(*game, "AppPlatform");
    note("{} AppPlatform classes", platforms.size());
    ClassInfo const* basePlatform = nullptr;
    for (auto const& c : platforms) {
        note("class {}: type_info {}, {} vtable(s)", c.name,
             c.typeInfo ? fmt::format("+0x{:X}", c.typeInfo - game->base) : "not found", c.vtables.size());
        if (c.name == "11AppPlatform" && !c.vtables.empty()) basePlatform = &c;
    }
    uintptr_t* baseVt = basePlatform ? basePlatform->vtables.front() : nullptr;
    // the platform classes' vtables (AppPlatform in full, the others where they differ from it)
    size_t baseCount = baseVt ? countSlots(*game, baseVt, 2000) : 0;
    for (auto const& c : platforms)
        for (auto* vt : c.vtables) {
            if (countSlots(*game, vt, 2000) < baseCount) continue;
            note("class {}: {} slots", c.name, countSlots(*game, vt, 2000));
        }

    for (char const* needle : {"MinecraftGame", "SplitscreenJoinListener"}) {
        auto classes = findClasses(*game, needle);
        note("{} classes named *{}*", classes.size(), needle);
        for (auto const& c : classes) {
            if (std::string_view(needle) == "MinecraftGame" && c.name != "13MinecraftGame") continue;
            if (std::string_view(needle) == "MinecraftGame"
                && (c.name.find("__function") != std::string::npos || c.name.find("shared_ptr") != std::string::npos))
                continue;
            note("class {}: {} vtable(s)", c.name, c.vtables.size());
        }
    }

    // 3. allowSplitScreen
    if (!baseVt) return note("ERROR: the AppPlatform vtable was not found, split screen stays off");
    size_t slot  = SIZE_MAX;
    size_t count = countSlots(*game, baseVt, 2000);
    uintptr_t allowSplitScreen = 0;
    if (!allowSplitScreenFns.empty()) {
        for (auto const& c : platforms)
            for (auto* vt : c.vtables)
                for (size_t i = 0, n = countSlots(*game, vt, 2000); i < n && slot == SIZE_MAX; ++i)
                    if (allowSplitScreenFns.contains(vt[i])) slot = i;
        if (slot != SIZE_MAX && slot < count) allowSplitScreen = baseVt[slot];
        else slot = SIZE_MAX;
        note("allowSplitScreen by symbol: slot {}", slot == SIZE_MAX ? std::string("not in the vtable") : std::to_string(slot));
    }
    if (slot == SIZE_MAX) {
        if (auto found = findAllowSplitScreenSlot(*game, baseVt, count)) {
            slot             = *found;
            allowSplitScreen = baseVt[slot];
            note("allowSplitScreen: slot {} ({})", slot, describeFunction(*game, allowSplitScreen));
        }
    }
    if (slot == SIZE_MAX) {
        note("ERROR: allowSplitScreen was not found, split screen stays off. Please send this log.");
        return;
    }
    if (!configFlag("forceSplitScreen", true)) return note("forceSplitScreen is off in config.json");
    gOriginalAllowSplitScreen = reinterpret_cast<AllowSplitScreenFn>(allowSplitScreen);
    note("turning split screen on (AppPlatform slot {})", slot);
    patchSlotEverywhere(*game, platforms, count, slot, allowSplitScreen, "allowSplitScreen");
    startFormProbe(*game, [](std::string const& l) { note("{}", l); });
    if (configFlag("forceSplitscreenJoin", true)) enableJoining(*game);
    else note("forceSplitscreenJoin is off in config.json");
}

std::once_flag gStarted;

void watchGame() {
    using namespace std::chrono_literals;
    uint64_t reported = UINT64_MAX;
    int      adding   = -1;
    for (int i = 0; i < 360; ++i) { // an hour
        std::this_thread::sleep_for(10s);
        uint64_t calls = gAllowSplitScreenCalls;
        if (calls != reported) note("allowSplitScreen asked {} time(s) so far", reported = calls);
        // MinecraftGame+1480: set while _addSubClientInstanceAndAssociateToUserInternal is adding a player
        if (void* game = gMinecraftGame.load()) {
            int now = *(reinterpret_cast<uint8_t const*>(game) + 1480);
            if (now != adding) note("join: MinecraftGame+1480 (adding a player) = {}", adding = now);
        }
    }
}

// The mod's resource pack (menu look, the black "Choose your player" screen, texts in every language) comes in the
// mod's own folder (pack/). It is put into the game's resource_packs folder and turned on as a global resource pack;
// the menus read their texts from it. The game's folder (games/com.mojang) is not in a fixed place under
// LeviLauncher: it is the one the game itself opens files in (/proc/self/fd), remembered in the mod's data folder
// for the next start, when the pack is put in before the game reads its packs.
constexpr char const* kPackUuid = "64c369bd-945a-4e1b-ab9a-4b3377148744";
namespace fs                    = std::filesystem;

fs::path packSource() { return SplitScreenMod::instance().getSelf().getModDir() / "pack"; }
fs::path rememberedFolderFile() { return SplitScreenMod::instance().getSelf().getDataDir() / "game_folder.txt"; }

// games/com.mojang from a path the game has open, e.g. ".../games/com.mojang/minecraftpe/options.txt"
fs::path gameFolderIn(std::string const& path) {
    auto at = path.find("/games/com.mojang/");
    return at == std::string::npos ? fs::path() : fs::path(path.substr(0, at + strlen("/games/com.mojang")));
}

fs::path openGameFolder() {
    std::error_code ec;
    for (auto const& fd : fs::directory_iterator("/proc/self/fd", ec)) {
        auto target = fs::read_symlink(fd.path(), ec);
        if (ec) continue;
        if (auto folder = gameFolderIn(target.string()); !folder.empty()) return folder;
    }
    return {};
}

void turnOnPack(fs::path const& folder) {
    std::error_code ec;
    if (!fs::exists(packSource() / "manifest.json", ec)) return note("pack: the mod's resource pack is not in its folder");
    fs::path packDir = folder / "resource_packs" / "SplitScreenUI";
    fs::create_directories(packDir, ec);
    fs::copy(packSource(), packDir, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    note("pack: copied to {}{}", packDir.string(), ec ? " (ERROR: " + ec.message() + ")" : "");

    fs::path    globalFile = folder / "minecraftpe" / "global_resource_packs.json";
    std::string text;
    {
        std::ifstream     in(globalFile);
        std::stringstream ss;
        ss << in.rdbuf();
        text = ss.str();
    }
    if (text.find(kPackUuid) != std::string::npos) return note("pack: turned on in {}", globalFile.string());
    std::string entry = fmt::format("{{\"pack_id\":\"{}\",\"version\":[1,0,0]}}", kPackUuid);
    auto        open  = text.find('[');
    if (open == std::string::npos) text = "[" + entry + "]";
    else {
        auto next = text.find_first_not_of(" \t\r\n", open + 1);
        text.insert(open + 1, next != std::string::npos && text[next] == ']' ? entry : entry + ",");
    }
    std::ofstream out(globalFile, std::ios::trunc);
    out << text;
    note("pack: turned on in {}{}", globalFile.string(), out ? " (from the next start of the game)" : " (ERROR: could not write)");
}

// At load: the folder found last time.
void installResourcePack() {
    std::ifstream in(rememberedFolderFile());
    std::string   folder;
    std::getline(in, folder);
    if (folder.empty()) return note("pack: the game's folder is not known yet (found once the game runs)");
    note("pack: game folder {}", folder);
    turnOnPack(folder);
    menus::setTexts(packSource(), fs::path(folder) / "minecraftpe" / "options.txt");
}

// While the game runs: the folder it has files open in; a new one is remembered and gets the pack.
void findGameFolder() {
    static fs::path known = [] {
        std::ifstream in(rememberedFolderFile());
        std::string   folder;
        std::getline(in, folder);
        return fs::path(folder);
    }();
    fs::path folder = openGameFolder();
    if (folder.empty() || folder == known) return;
    known = folder;
    note("pack: the game's folder is {}", folder.string());
    std::error_code ec;
    fs::create_directories(rememberedFolderFile().parent_path(), ec);
    std::ofstream(rememberedFolderFile(), std::ios::trunc) << folder.string() << "\n";
    turnOnPack(folder);
    menus::setTexts(packSource(), folder / "minecraftpe" / "options.txt");
}

void watchControllers() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        menus::checkControllers();
        static int tick = 0;
        if (tick++ % 10 == 0) findGameFolder();
    }
}

} // namespace

SplitScreenMod& SplitScreenMod::instance() {
    static SplitScreenMod mod;
    return mod;
}

bool SplitScreenMod::load() {
    std::error_code ignored;
    std::filesystem::create_directories(getSelf().getModDir(), ignored);
    auto log = getSelf().getModDir() / "splitscreen.log";
    std::filesystem::rename(log, getSelf().getModDir() / "splitscreen.prev.log", ignored);
    gLog.open(log, std::ios::trunc);
    openDownloadLog();
    note("SplitScreenMod for Android {} (experimental)", SPLITSCREEN_VERSION);
    installResourcePack();
    return true;
}

bool SplitScreenMod::enable() {
    std::call_once(gStarted, [] {
        std::thread([] {
            probe();
            std::thread(watchControllers).detach();
            watchGame();
        }).detach();
    });
    return true;
}

bool SplitScreenMod::disable() {
    for (auto [slot, original] : gPatched) writeSlot(slot, original);
    gPatched.clear();
    return true;
}

} // namespace splitscreen
