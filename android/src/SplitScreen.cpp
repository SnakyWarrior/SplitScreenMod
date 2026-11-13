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


#include <dlfcn.h>

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

void forgetPlayers(char const* why, int sub) {
    std::lock_guard lock(gPlayingMutex);
    if (gPlaying.empty()) return;
    if (!gPlaying.erase(sub)) return;
    note("join: {}({}): player {} is gone", why, sub, sub + 1);
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

void addSubDetour(void* self, int controller) {
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
    note("join: controller {} pressed Start: adding player {}", controller, controller + 1);
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
    addSubDetour(game, controller);
    tAddingPlayer = false;
}

// The Split screen menu, on Select (View / Back) of a controller that plays. The added player's sub client id is its
// controller number (1-3).
void openSplitScreenMenu(int controller) {
    {
        std::lock_guard lock(gPlayingMutex);
        if (!gPlaying.contains(controller)) return;
    }
    note("menus: player {} opens the Split screen menu", controller + 1);
    showMenu(controller, "Split screen", fmt::format("Player {}", controller + 1), {"Leave split screen", "Cancel"},
             [controller](int button) {
                 if (button != 0) return;
                 // Removed at once, the player's client goes while its form screen is still closing, and the game
                 // crashes on a destroyed mutex: wait for the screen to close.
                 note("menus: player {} leaves split screen in 1.5 seconds", controller + 1);
                 runOnInputThread(
                     [controller] {
                         void* game = gMinecraftGame.load();
                         note("menus: removing player {}", controller + 1);
                         if (game && gQueueRemoval) queueRemovalDetour(game, controller);
                     },
                     std::chrono::milliseconds(1500));
             });
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
    // The game's own "player leaves" path (1.21.x): the code before its queueSubclientRemoval call.
    dumpCode(game, "leave", game.base + 0x9B7B540 - 0x600, 0x640, [](std::string const& line) { note("{}", line); });
    writeSlot(&vt[16], reinterpret_cast<uintptr_t>(&releaseSubIdDetour));
    if (ok) onStartPressed(joinController);
    if (ok) onSelectPressed(openSplitScreenMenu);
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
            if (c.name.find("__function") != std::string::npos || c.name.find("shared_ptr") != std::string::npos) continue;
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
    return true;
}

bool SplitScreenMod::enable() {
    std::call_once(gStarted, [] {
        std::thread([] {
            probe();
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
