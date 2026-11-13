#include "ControllerSplit.h"

#include "DeviceManagerProbe.h"

#include <pl/memory/Hook.hpp>

#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <chrono>
#include <set>
#include <vector>

#include <fmt/format.h>

extern "C" {
uintptr_t splitscreen_originals[16];
void      splitscreen_trampoline_0();
void      splitscreen_trampoline_1();
void      splitscreen_trampoline_2();
void      splitscreen_trampoline_3();
void      splitscreen_trampoline_4();
void      splitscreen_trampoline_5();
void      splitscreen_trampoline_6();
void      splitscreen_trampoline_7();
void      splitscreen_trampoline_8();
void      splitscreen_trampoline_9();
void      splitscreen_trampoline_10();
void      splitscreen_trampoline_11();
void      splitscreen_trampoline_12();
void      splitscreen_trampoline_13();
void      splitscreen_trampoline_14();
void      splitscreen_trampoline_15();
}

namespace splitscreen {

namespace {

constexpr int kMaxControllers = 4;
constexpr int kMaxHooks       = 16;

Module const* gGame = nullptr;
LogFn         gLog;
uintptr_t     gMethods[kMaxHooks];
std::atomic<uint64_t> gCalls[kMaxHooks];
std::atomic<int>      gRemapsLogged{0};

// device id -> controller number; the first controller gets 0 (player 1)
std::mutex                gDevicesMutex;
std::map<int32_t, int>    gControllerOf;

thread_local int tCurrentController = -1;

// The first controller number a controller can get: 1 keeps player 1 on touch (every controller joins as a new
// player, as on Windows player 1 keeps keyboard and mouse); 0 gives the first controller to player 1.
std::atomic<int> gFirstController{1};

std::function<std::vector<int>()> gPlayingFn;
std::function<void(int)>          gTookOverFn;
std::vector<int>                  gTookOver; // lost players a new controller took over, told after the event

int controllerFor(int32_t device) {
    std::lock_guard lock(gDevicesMutex);
    if (auto it = gControllerOf.find(device); it != gControllerOf.end()) return it->second;
    std::array<bool, kMaxControllers> used{};
    for (auto [d, c] : gControllerOf) used[c] = true;
    // a player who lost their controller gets the new one first
    if (gPlayingFn)
        for (int c : gPlayingFn())
            if (c >= gFirstController && c < kMaxControllers && !used[c]) {
                gControllerOf[device] = c;
                gLog(fmt::format("controllers: device {} is controller {}: it takes over player {}, who had none", device,
                                 c, c + 1));
                gTookOver.push_back(c);
                return c;
            }
    for (int c = gFirstController; c < kMaxControllers; ++c)
        if (!used[c]) {
            gControllerOf[device] = c;
            gLog(fmt::format("controllers: device {} is controller {}{}", device, c,
                             c == 0 ? " (player 1)" : " (joins with Start as a new player)"));
            return c;
        }
    return -1; // more than 4: leave it on controller 0 as the game does
}

void releaseDevice(int32_t device) {
    std::lock_guard lock(gDevicesMutex);
    if (gControllerOf.erase(device)) gLog(fmt::format("controllers: device {} removed", device));
}

// Reads memory without crashing on a bad pointer.
bool safeRead(uintptr_t address, void* out, size_t size) {
    iovec local{out, size}, remote{reinterpret_cast<void*>(address), size};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == ssize_t(size);
}

constexpr int32_t kSourceGamepad  = 0x00000401;
constexpr int32_t kSourceJoystick = 0x01000010;

std::function<void(int)> gStartPressed;

// GameActivityKeyEvent: deviceId, source, action, (pad), eventTime, downTime, flags, metaState, modifiers,
// repeatCount, keyCode, scanCode, ... (checked on a device: the int at index 12 is the Android key code)
constexpr int kKeyCodeIndex   = 12;
constexpr int kKeyCodeStart   = 108; // KEYCODE_BUTTON_START
constexpr int kKeyCodeSelect  = 109; // KEYCODE_BUTTON_SELECT (View / Back / Share / -)

std::function<void(int)>           gSelectPressed;
std::mutex                         gTasksMutex;
struct Task {
    std::chrono::steady_clock::time_point due;
    std::function<void()>                 run;
};
std::vector<Task> gTasks; // run on the input thread, at the first input event after they are due
constexpr int kKeyActionDown  = 0;

// --- dispatch ---------------------------------------------------------------------------------------------------
using Fn8 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
Fn8              gDispatch = nullptr;
std::atomic<int> gEventsLogged{0};

uint64_t dispatchDetour(uint64_t a0, uint64_t a1, uint64_t variant, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6,
                        uint64_t a7) {
    int       controller = -1;
    uintptr_t event      = 0;
    int32_t   index = -1, head[2] = {0, 0};
    bool      startDown = false, selectDown = false;
    if (safeRead(variant + 8, &index, 4) && index != -1 && safeRead(variant, &event, 8) && event
        && safeRead(event, head, 8)) {
        int32_t key[kKeyCodeIndex + 1] = {};
        if (index == 0 && safeRead(event, key, sizeof key))
        {
            startDown  = key[kKeyCodeIndex] == kKeyCodeStart && key[2] == kKeyActionDown;
            selectDown = key[kKeyCodeIndex] == kKeyCodeSelect && key[2] == kKeyActionDown;
        }
        bool pad = (head[1] & kSourceGamepad) == kSourceGamepad || (head[1] & kSourceJoystick) == kSourceJoystick;
        if (pad) controller = controllerFor(head[0]);
        if (gEventsLogged < 24) {
            ++gEventsLogged;
            gLog(fmt::format("controllers: event kind {} device {} source 0x{:X} -> controller {}", index, head[0],
                             head[1], controller));
        }
    }
    int previous       = tCurrentController;
    tCurrentController = controller;
    uint64_t result    = gDispatch(a0, a1, variant, a3, a4, a5, a6, a7);
    tCurrentController = previous;
    std::vector<int> tookOver;
    {
        std::lock_guard lock(gDevicesMutex);
        tookOver.swap(gTookOver);
    }
    if (gTookOverFn)
        for (int c : tookOver) gTookOverFn(c);
    if (startDown && controller >= 1 && gStartPressed) gStartPressed(controller);
    if (selectDown && controller >= 1 && gSelectPressed) gSelectPressed(controller);
    std::vector<std::function<void()>> tasks;
    {
        std::lock_guard lock(gTasksMutex);
        auto now = std::chrono::steady_clock::now();
        for (auto it = gTasks.begin(); it != gTasks.end();)
            if (it->due <= now) {
                tasks.push_back(std::move(it->run));
                it = gTasks.erase(it);
            } else
                ++it;
    }
    for (auto& task : tasks) task();
    return result;
}

// --- the manager's add / remove device (manager, int device) ------------------------------------------------------
Fn8 gAdded = nullptr, gRemoved = nullptr;

uint64_t addedDetour(uint64_t a0, uint64_t device, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6,
                     uint64_t a7) {
    int previous       = tCurrentController;
    tCurrentController = controllerFor(int32_t(device));
    uint64_t result    = gAdded(a0, device, a2, a3, a4, a5, a6, a7);
    tCurrentController = previous;
    return result;
}

uint64_t removedDetour(uint64_t a0, uint64_t device, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6,
                       uint64_t a7) {
    int previous;
    {
        std::lock_guard lock(gDevicesMutex);
        auto it  = gControllerOf.find(int32_t(device));
        previous = tCurrentController;
        tCurrentController = it == gControllerOf.end() ? -1 : it->second;
    }
    uint64_t result    = gRemoved(a0, device, a2, a3, a4, a5, a6, a7);
    tCurrentController = previous;
    releaseDevice(int32_t(device));
    return result;
}

// --- finding things in the code -----------------------------------------------------------------------------------
bool isAdrp(uint32_t op, int rd) { return (op & 0x9F00001F) == (0x90000000u | uint32_t(rd)); }
uintptr_t adrpTarget(uintptr_t pc, uint32_t op) {
    int64_t imm = (int64_t(((op >> 5) & 0x7FFFF) << 2 | ((op >> 29) & 3)) << 43) >> 31;
    return (pc & ~uintptr_t(0xFFF)) + imm;
}
bool isAddImm(uint32_t op, int rd, int rn) {
    return (op & 0xFF80001F) == (0x91000000u | uint32_t(rd)) && int((op >> 5) & 31) == rn;
}
uintptr_t addImm(uint32_t op) { return uintptr_t((op >> 10) & 0xFFF) << (((op >> 22) & 1) ? 12 : 0); }
bool      isBranch(uint32_t op) { return (op & 0x7C000000) == 0x14000000; } // b or bl
uintptr_t branchTarget(uintptr_t pc, uint32_t op) { return pc + ((int64_t(op & 0x3FFFFFF) << 38) >> 36); }

// dispatch is just before the manager's add function: the function whose code has "ldr w8, [x2, #8]" (the
// variant's index) and "cmn w8, #1" (valueless_by_exception), starting at the nearest "sub sp, sp, #n".
uintptr_t findDispatch(Module const& game, uintptr_t managerAdded, uintptr_t& table) {
    auto const* code = reinterpret_cast<uint32_t const*>(managerAdded - 0x400);
    for (int i = 0x100 - 1; i >= 0; --i) {
        if (code[i] != 0xB9400848) continue; // ldr w8, [x2, #8]
        bool cmn = false;
        for (int k = i + 1; k < i + 6; ++k) cmn |= code[k] == 0x3100051F; // cmn w8, #1
        if (!cmn) continue;
        int start = -1;
        for (int k = i; k > i - 24 && k >= 0; --k)
            if ((code[k] & 0xFF0003FF) == 0xD10003FF) { // sub sp, sp, #n
                start = k;
                break;
            }
        if (start < 0) continue;
        // its visitor table: "adrp x10; add x10, x10, #n"
        table = 0;
        for (int k = i; k < i + 16; ++k)
            if (isAdrp(code[k], 10) && isAddImm(code[k + 1], 10, 10)) {
                table = adrpTarget(managerAdded - 0x400 + 4 * k, code[k]) + addImm(code[k + 1]);
                break;
            }
        uintptr_t at = managerAdded - 0x400 + 4 * start;
        return game.isCode(at) ? at : 0;
    }
    return 0;
}

// Calls on `object` ("adrp x0; add x0, x0" then a b/bl) in `size` bytes of code from `from`.
void collectCalls(Module const& game, uintptr_t from, size_t size, uintptr_t object, std::vector<uintptr_t>& out) {
    if (!game.isCode(from) || !game.isCode(from + size)) return;
    auto const* code = reinterpret_cast<uint32_t const*>(from);
    int         n    = int(size / 4);
    for (int i = 0; i + 1 < n; ++i) {
        if (!isAdrp(code[i], 0) || !isAddImm(code[i + 1], 0, 0)) continue;
        if (adrpTarget(from + 4 * i, code[i]) + addImm(code[i + 1]) != object) continue;
        for (int j = i + 2; j < i + 14 && j < n; ++j)
            if (isBranch(code[j])) {
                uintptr_t target = branchTarget(from + 4 * j, code[j]);
                if (game.isCode(target) && std::find(out.begin(), out.end(), target) == out.end()) out.push_back(target);
                break;
            }
    }
}

std::string offset(uintptr_t p) { return fmt::format("+0x{:X}", p - gGame->base); }

} // namespace

void onStartPressed(std::function<void(int)> handler) { gStartPressed = std::move(handler); }

void onSelectPressed(std::function<void(int)> handler) { gSelectPressed = std::move(handler); }

void runOnInputThread(std::function<void()> task, std::chrono::milliseconds delay) {
    std::lock_guard lock(gTasksMutex);
    gTasks.push_back(Task{std::chrono::steady_clock::now() + delay, std::move(task)});
}

void setFirstControllerIsPlayer1(bool yes) { gFirstController = yes ? 0 : 1; }

void setPlayingPlayers(std::function<std::vector<int>()> playing) { gPlayingFn = std::move(playing); }

void onLostPlayerTakenOver(std::function<void(int)> handler) { gTookOverFn = std::move(handler); }

bool controllerConnected(int controller) {
    std::lock_guard lock(gDevicesMutex);
    for (auto [d, c] : gControllerOf)
        if (c == controller) return true;
    return false;
}

void swapControllers(int a, int b) {
    std::lock_guard lock(gDevicesMutex);
    for (auto& [d, c] : gControllerOf)
        if (c == a) c = b;
        else if (c == b) c = a;
    gLog(fmt::format("controllers: controllers {} and {} swapped", a, b));
}

bool startControllerSplit(Module const& game, uintptr_t managerObject, uintptr_t managerAdded, uintptr_t managerRemoved,
                          LogFn log) {
    static Module copy = game;
    gGame              = &copy;
    gLog               = std::move(log);

    uintptr_t table    = 0;
    uintptr_t dispatch = findDispatch(copy, managerAdded, table);
    if (!dispatch) return gLog("controllers: ERROR: the controller manager's dispatch function was not found"), false;
    gLog(fmt::format("controllers: dispatch {} (visitors at {})", offset(dispatch), table ? offset(table) : "?"));

    // GameControllerManager and its functions
    auto system = findGamepadSystem(copy, managerAdded, managerObject);
    if (!system.object) return gLog("controllers: ERROR: GameControllerManager not found"), false;
    std::vector<uintptr_t> methods = system.methods;
    if (table) {
        for (int v = 0; v < 8; ++v) {
            uintptr_t visitor = 0;
            if (!safeRead(table + 8 * v, &visitor, 8) || !copy.isCode(visitor)) break;
            collectCalls(copy, visitor, 0x1800, system.object, methods);
            // visitors are often thunks: also scan what they call first
            auto const* code = reinterpret_cast<uint32_t const*>(visitor);
            for (int k = 0; k < 16; ++k)
                if (isBranch(code[k])) {
                    collectCalls(copy, branchTarget(visitor + 4 * k, code[k]), 0x1800, system.object, methods);
                    break;
                }
        }
    }
    gLog(fmt::format("controllers: GameControllerManager {} ({}), {} functions", offset(system.object),
                     className(copy, system.object), methods.size()));

    void (*trampolines[kMaxHooks])() = {
        splitscreen_trampoline_0,  splitscreen_trampoline_1,  splitscreen_trampoline_2,  splitscreen_trampoline_3,
        splitscreen_trampoline_4,  splitscreen_trampoline_5,  splitscreen_trampoline_6,  splitscreen_trampoline_7,
        splitscreen_trampoline_8,  splitscreen_trampoline_9,  splitscreen_trampoline_10, splitscreen_trampoline_11,
        splitscreen_trampoline_12, splitscreen_trampoline_13, splitscreen_trampoline_14, splitscreen_trampoline_15};
    for (size_t i = 0; i < methods.size() && i < kMaxHooks; ++i) {
        gMethods[i] = methods[i];
        bool ok     = pl::memory::hook(reinterpret_cast<pl::memory::FuncPtr>(methods[i]),
                                   reinterpret_cast<pl::memory::FuncPtr>(trampolines[i]),
                                   reinterpret_cast<pl::memory::FuncPtr*>(&splitscreen_originals[i]))
                 == 0;
        gLog(fmt::format("controllers: #{} {} {}", i, offset(methods[i]), ok ? "hooked" : "ERROR: not hooked"));
    }

    auto hook = [&](uintptr_t target, void* detour, Fn8* original, char const* name) {
        bool ok = pl::memory::hook(reinterpret_cast<pl::memory::FuncPtr>(target), detour,
                                   reinterpret_cast<pl::memory::FuncPtr*>(original))
               == 0;
        gLog(fmt::format("controllers: {} {} {}", name, offset(target), ok ? "hooked" : "ERROR: not hooked"));
        return ok;
    };
    bool ok = hook(dispatch, reinterpret_cast<void*>(&dispatchDetour), &gDispatch, "dispatch");
    if (managerAdded) hook(managerAdded, reinterpret_cast<void*>(&addedDetour), &gAdded, "device added");
    if (managerRemoved) hook(managerRemoved, reinterpret_cast<void*>(&removedDetour), &gRemoved, "device removed");
    return ok;
}

} // namespace splitscreen

// Called by the trampolines (Trampolines.S) on every GameControllerManager call, with the saved x0..x8.
extern "C" void splitscreen_on_call(uint64_t index, uint64_t* regs, uint64_t, uint64_t ret) {
    using namespace splitscreen;
    uint64_t n = ++gCalls[index];
    if (n == 1 || n == 100 || n == 10000)
        gLog(fmt::format("controllers: #{} called {} times (first from {})", index, n, offset(ret)));
    int controller = tCurrentController;
    if (controller > 0 && int32_t(regs[1]) == 0) {
        regs[1] = (regs[1] & ~uint64_t(0xFFFFFFFF)) | uint32_t(controller);
        if (gRemapsLogged < 30) {
            ++gRemapsLogged;
            gLog(fmt::format("controllers: #{} ({}) controller 0 -> {}, args {} {} {}", index, offset(gMethods[index]),
                             controller, int32_t(regs[2]), int32_t(regs[3]), int32_t(regs[4])));
        }
    }
}
