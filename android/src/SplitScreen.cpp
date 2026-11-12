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
#include "InputProbe.h"


#include <dlfcn.h>

#include <atomic>
#include <cctype>
#include <chrono>
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
        // the code of the input JNI functions, to read offline
        for (auto const& e : exports)
            for (char const* name : {"MainActivity_nativeKeyHandler", "JellyBeanDeviceManager_onInputDeviceAddedNative",
                                     "JellyBeanDeviceManager_onInputDeviceRemovedNative",
                                     "JellyBeanDeviceManager_setControllerDetailsNative",
                                     "JellyBeanDeviceManager_onInputDeviceChangedNative"})
                if (e.name.ends_with(name)) dumpCode(*game, name, e.address, 512, [](std::string const& l) { note("{}", l); });
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
    startInputProbe(*game, [](std::string const& l) { note("{}", l); });

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

    // the game's controller classes (names only)
    for (char const* needle : {"Gamepad", "GamePad", "Controller", "InputDevice", "DeviceManager"}) {
        int shown = 0;
        for (auto const& t : findTypeNames(*game, needle)) {
            if (t.name.starts_with("NSt6__ndk1") || t.name.find("Screen") != std::string::npos
                || t.name.find("ZN") != std::string::npos)
                continue;
            if (++shown > 80) break;
            note("type *{}*: {}", needle, t.name);
        }
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
    dumpCode(*game, "allowSplitScreen", allowSplitScreen, 256, [](std::string const& l) { note("{}", l); });
    patchSlotEverywhere(*game, platforms, count, slot, allowSplitScreen, "allowSplitScreen");
}

std::once_flag gStarted;

void watchGame() {
    using namespace std::chrono_literals;
    uint64_t reported = UINT64_MAX;
    for (int i = 0; i < 360; ++i) { // an hour
        std::this_thread::sleep_for(10s);
        uint64_t calls = gAllowSplitScreenCalls;
        if (calls == reported) continue;
        reported = calls;
        note("allowSplitScreen asked {} time(s) so far", calls);
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
