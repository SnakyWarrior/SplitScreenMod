#include "DeviceManagerProbe.h"

#include <chrono>
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
