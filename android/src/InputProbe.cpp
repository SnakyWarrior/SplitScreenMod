#include "InputProbe.h"

#include <android/input.h>
#include <dlfcn.h>

#include <atomic>
#include <map>
#include <mutex>
#include <set>

#include <fmt/format.h>

namespace splitscreen {

namespace {

Module const* gGame = nullptr;
LogFn         gLog;
std::mutex    gMutex;

using GetDeviceIdFn = int32_t (*)(AInputEvent const*);
GetDeviceIdFn gGetDeviceId = nullptr;

std::set<int32_t>   gDevicesSeen;
std::set<uintptr_t> gCallersSeen;
std::atomic<int>    gKeysLogged{0};

bool isGamepadKey(int32_t code) { return (code >= 96 && code <= 110) || (code >= 188 && code <= 203); }

std::string sourceName(int32_t source) {
    std::string out;
    if ((source & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD) out += "gamepad ";
    if ((source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) out += "joystick ";
    if ((source & AINPUT_SOURCE_KEYBOARD) == AINPUT_SOURCE_KEYBOARD) out += "keyboard ";
    if ((source & AINPUT_SOURCE_TOUCHSCREEN) == AINPUT_SOURCE_TOUCHSCREEN) out += "touchscreen ";
    if ((source & AINPUT_SOURCE_MOUSE) == AINPUT_SOURCE_MOUSE) out += "mouse ";
    if ((source & AINPUT_SOURCE_DPAD) == AINPUT_SOURCE_DPAD) out += "dpad ";
    return out + fmt::format("(0x{:X})", source);
}

int32_t getDeviceIdWrapper(AInputEvent const* event) {
    int32_t   id     = gGetDeviceId(event);
    uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    bool      newCaller = false;
    {
        std::lock_guard lock(gMutex);
        int32_t source = AInputEvent_getSource(event);
        if (gDevicesSeen.insert(id).second)
            gLog(fmt::format("input: first event from device {}: {}", id, sourceName(source)));
        newCaller = gGame && gGame->isCode(caller) && gCallersSeen.size() < 16 && gCallersSeen.insert(caller).second;
        if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY && gKeysLogged < 300) {
            int32_t code = AKeyEvent_getKeyCode(event);
            if (isGamepadKey(code)) {
                ++gKeysLogged;
                gLog(fmt::format("input: device {} key {} {} (asked from +0x{:X})", id, code,
                                 AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_DOWN ? "down" : "up",
                                 gGame ? caller - gGame->base : caller));
            }
        }
    }
    if (newCaller) {
        gLog(fmt::format("input: device id asked from +0x{:X}", caller - gGame->base));
        dumpCode(*gGame, "caller", caller - 512, 1024, gLog);
    }
    return id;
}

} // namespace

void dumpCode(Module const& game, char const* label, uintptr_t address, size_t size, LogFn const& log) {
    if (!game.isCode(address) || !game.isCode(address + size - 1)) return log(fmt::format("code {}: not in the game", label));
    auto const* bytes = reinterpret_cast<uint8_t const*>(address);
    for (size_t off = 0; off < size; off += 32) {
        std::string line = fmt::format("code {} +0x{:X}:", label, address + off - game.base);
        for (size_t i = off; i < off + 32 && i < size; ++i) line += fmt::format(" {:02x}", bytes[i]);
        log(line);
    }
}

void startInputProbe(Module const& game, LogFn log) {
    static Module copy = game;
    gGame              = &copy;
    gLog               = std::move(log);

    void* android = dlopen("libandroid.so", RTLD_NOW | RTLD_NOLOAD);
    auto  target  = android ? reinterpret_cast<uintptr_t>(dlsym(android, "AInputEvent_getDeviceId")) : 0;
    if (!target) return gLog("input: AInputEvent_getDeviceId not found");
    gGetDeviceId = reinterpret_cast<GetDeviceIdFn>(target);

    // its GOT entries in the game library: pointer-sized words in the writable segments equal to the function
    int patched = 0;
    for (auto const& s : copy.segments) {
        if (!s.read || !s.write) continue;
        for (uintptr_t p = (s.start + 7) & ~uintptr_t(7); p + 8 <= s.end; p += 8) {
            auto* slot = reinterpret_cast<uintptr_t*>(p);
            if (*slot != target) continue;
            if (writeSlot(slot, reinterpret_cast<uintptr_t>(&getDeviceIdWrapper))) {
                ++patched;
                gLog(fmt::format("input: watching AInputEvent_getDeviceId (GOT +0x{:X})", p - copy.base));
            }
        }
    }
    if (!patched) gLog("input: AInputEvent_getDeviceId is not in the game's GOT; controller ids are read another way");
}

} // namespace splitscreen
