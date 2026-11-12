#include "DeviceManagerProbe.h"

#include <pl/memory/Hook.hpp>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <algorithm>
#include <cstring>
#include <thread>

#include <fmt/format.h>

namespace splitscreen {

namespace {

// AArch64 decoding of the few instructions needed here.
bool isAdrp(uint32_t op, int rd) { return (op & 0x9F00001F) == (0x90000000u | uint32_t(rd)); }
uintptr_t adrpTarget(uintptr_t pc, uint32_t op) {
    int64_t imm = (int64_t(((op >> 5) & 0x7FFFF) << 2 | ((op >> 29) & 3)) << 43) >> 31; // sign-extend 21 bits, << 12
    return (pc & ~uintptr_t(0xFFF)) + imm;
}
// add xd, xn, #imm (64-bit, optional lsl #12)
bool isAddImm(uint32_t op, int rd, int rn) {
    return (op & 0xFF80001F) == (0x91000000u | uint32_t(rd)) && int((op >> 5) & 31) == rn;
}
uintptr_t addImm(uint32_t op) { return uintptr_t((op >> 10) & 0xFFF) << (((op >> 22) & 1) ? 12 : 0); }
bool      isBl(uint32_t op) { return (op & 0xFC000000) == 0x94000000; }
uintptr_t blTarget(uintptr_t pc, uint32_t op) { return pc + ((int64_t(op & 0x3FFFFFF) << 38) >> 36); }

} // namespace

std::vector<DeviceManagerCall> findDeviceManagerCalls(Module const& game, std::vector<Symbol> const& exports) {
    (void)game;
    std::vector<DeviceManagerCall> out;
    for (char const* jni : {"onInputDeviceAddedNative", "onInputDeviceRemovedNative", "setControllerDetailsNative",
                            "onInputDeviceChangedNative"}) {
        for (auto const& e : exports) {
            if (!e.name.starts_with("Java_com_mojang_minecraftpe_input_JellyBeanDeviceManager_") || !e.name.ends_with(jni))
                continue;
            auto const* code   = reinterpret_cast<uint32_t const*>(e.address);
            uintptr_t   object = 0;
            for (int i = 0; i + 1 < 64; ++i) {
                uintptr_t pc = e.address + 4 * i;
                if (code[i] == 0xD65F03C0 && object == 0) break; // ret before the pattern: not this shape
                if (isAdrp(code[i], 0) && isAddImm(code[i + 1], 0, 0)) {
                    object = adrpTarget(pc, code[i]) + addImm(code[i + 1]);
                    continue;
                }
                if (object && isBl(code[i])) {
                    out.push_back({jni, object, blTarget(pc, code[i])});
                    break;
                }
            }
        }
    }
    return out;
}

GamepadSystem findGamepadSystem(Module const& game, uintptr_t managerMethod, uintptr_t managerObject) {
    GamepadSystem                  out;
    std::map<uintptr_t, int>       objects;
    std::vector<std::pair<uintptr_t, uintptr_t>> calls; // object, target
    auto const* code = reinterpret_cast<uint32_t const*>(managerMethod);
    constexpr int kScan = 1536; // instructions (6 KB) of the manager's code
    if (!game.isCode(managerMethod + 4 * kScan)) return out;
    for (int i = 0; i + 1 < kScan; ++i) {
        if (!isAdrp(code[i], 0) || !isAddImm(code[i + 1], 0, 0)) continue;
        uintptr_t pc     = managerMethod + 4 * i;
        uintptr_t object = adrpTarget(pc, code[i]) + addImm(code[i + 1]);
        if (object == managerObject) continue;
        for (int j = i + 2; j < i + 14 && j < kScan; ++j) {
            uint32_t op = code[j];
            bool     b  = (op & 0xFC000000) == 0x14000000;
            if (isBl(op) || b) {
                calls.push_back({object, blTarget(managerMethod + 4 * j, op)}); // b and bl encode the target alike
                ++objects[object];
                break;
            }
        }
    }
    int best = 0;
    for (auto [object, n] : objects)
        if (n > best) best = n, out.object = object;
    for (auto [object, target] : calls)
        if (object == out.object && std::find(out.methods.begin(), out.methods.end(), target) == out.methods.end())
            out.methods.push_back(target);
    return out;
}

std::string className(Module const& game, uintptr_t object) {
    auto vptr = *reinterpret_cast<uintptr_t const*>(object);
    bool inGame = false;
    for (auto const& s : game.segments) inGame |= vptr >= s.start + 8 && vptr < s.end && !s.exec;
    if (!inGame) return "";
    auto typeInfo = reinterpret_cast<uintptr_t const*>(vptr)[-1];
    for (auto const& s : game.segments)
        if (typeInfo >= s.start && typeInfo + 16 <= s.end) {
            auto name = reinterpret_cast<char const* const*>(typeInfo)[1];
            for (auto const& t : game.segments)
                if (reinterpret_cast<uintptr_t>(name) >= t.start && reinterpret_cast<uintptr_t>(name) < t.end)
                    return std::string(name, strnlen(name, 200));
        }
    return "";
}

void watchObject(Module const& game, uintptr_t object, size_t size, LogFn log) {
    std::thread([object, size, log, base = game.base] {
        using namespace std::chrono_literals;
        std::vector<uint8_t> last;
        for (int round = 0; round < 360; ++round) {
            std::vector<uint8_t> now(size);
            std::memcpy(now.data(), reinterpret_cast<void const*>(object), size);
            if (now != last) {
                log(fmt::format("object +0x{:X} at round {}:", object - base, round));
                for (size_t off = 0; off < size; off += 32) {
                    if (!last.empty() && std::memcmp(now.data() + off, last.data() + off, std::min<size_t>(32, size - off)) == 0)
                        continue; // only the lines that changed
                    std::string line = fmt::format("  +{:03X}:", off);
                    for (size_t i = off; i < off + 32 && i < size; i += 8)
                        line += fmt::format(" {:016X}", *reinterpret_cast<uint64_t const*>(now.data() + i));
                    log(line);
                }
                last = std::move(now);
            }
            std::this_thread::sleep_for(10s);
        }
    }).detach();
}

} // namespace splitscreen

extern "C" {
uintptr_t splitscreen_originals[8];
void      splitscreen_trampoline_0();
void      splitscreen_trampoline_1();
void      splitscreen_trampoline_2();
void      splitscreen_trampoline_3();
void      splitscreen_trampoline_4();
void      splitscreen_trampoline_5();
void      splitscreen_trampoline_6();
void      splitscreen_trampoline_7();
}

namespace splitscreen {
namespace {

Module const*             gCallGame = nullptr;
LogFn                     gCallLog;
uintptr_t                 gMethods[8];
std::atomic<uint64_t>     gCalls[8];
std::mutex                gCallMutex;
std::set<std::pair<uint64_t, uintptr_t>> gCallSites;

std::string offset(uintptr_t p) {
    return gCallGame->isCode(p) ? fmt::format("+0x{:X}", p - gCallGame->base) : fmt::format("0x{:X}", p);
}

} // namespace

void watchGamepadCalls(Module const& game, std::vector<uintptr_t> const& methods, LogFn log) {
    static Module copy = game;
    gCallGame          = &copy;
    gCallLog           = std::move(log);
    void (*trampolines[8])() = {splitscreen_trampoline_0, splitscreen_trampoline_1, splitscreen_trampoline_2,
                                splitscreen_trampoline_3, splitscreen_trampoline_4, splitscreen_trampoline_5,
                                splitscreen_trampoline_6, splitscreen_trampoline_7};
    for (size_t i = 0; i < methods.size() && i < 8; ++i) {
        gMethods[i] = methods[i];
        if (pl::memory::hook(reinterpret_cast<pl::memory::FuncPtr>(methods[i]),
                             reinterpret_cast<pl::memory::FuncPtr>(trampolines[i]),
                             reinterpret_cast<pl::memory::FuncPtr*>(&splitscreen_originals[i]))
            != 0)
            gCallLog(fmt::format("gamepad #{}: ERROR: could not hook {}", i, offset(methods[i])));
        else gCallLog(fmt::format("gamepad #{}: watching {}", i, offset(methods[i])));
    }
}

} // namespace splitscreen

// Called by the trampolines with the saved x0..x8, the caller's frame pointer and return address.
extern "C" void splitscreen_on_call(uint64_t index, uint64_t* regs, uint64_t fp, uint64_t ret) {
    using namespace splitscreen;
    uint64_t n = ++gCalls[index];
    if (n == 1 || n == 10 || n == 100 || n == 1000 || n == 10000 || n == 100000)
        gCallLog(fmt::format("gamepad #{}: called {} times", index, n));
    {
        std::lock_guard lock(gCallMutex);
        if (gCallSites.size() >= 48 || !gCallSites.insert({index, ret}).second) return;
    }
    std::string chain;
    uintptr_t   frame = fp;
    for (int k = 0; k < 10 && frame && (frame & 7) == 0; ++k) {
        auto const* f    = reinterpret_cast<uintptr_t const*>(frame);
        uintptr_t   next = f[0], lr = f[1];
        if (!lr) break;
        chain += " " + offset(lr);
        if (next <= frame || next - frame > (1u << 20)) break;
        frame = next;
    }
    gCallLog(fmt::format("gamepad #{} ({}): w1={} w2={} w3={} w4={} from {}; callers:{}", index, offset(gMethods[index]),
                         int32_t(regs[1]), int32_t(regs[2]), int32_t(regs[3]), int32_t(regs[4]), offset(ret), chain));
}
