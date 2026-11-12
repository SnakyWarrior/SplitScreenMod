#include "VTrace.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include <fmt/format.h>

namespace {
constexpr size_t kMaxSlots = 400;
}

extern "C" {
uintptr_t       splitscreen_voriginals[kMaxSlots];
extern uintptr_t splitscreen_vtrampolines[kMaxSlots];
}

namespace splitscreen {

namespace {

Module const*         gGame = nullptr;
LogFn                 gLog;
std::string           gNames[kMaxSlots]; // the class of each trampoline
size_t                gSlots[kMaxSlots]; // its vtable slot
std::atomic<bool>     gTracing{false};
std::atomic<uint64_t> gCounts[kMaxSlots];
std::atomic<bool>     gSeen[kMaxSlots];

std::string offset(uintptr_t p) {
    return gGame && gGame->isCode(p) ? fmt::format("+0x{:X}", p - gGame->base) : fmt::format("0x{:X}", p);
}

} // namespace

bool traceVtable(Module const& game, uintptr_t* vtable, size_t slots, size_t first, char const* name, LogFn log) {
    static Module copy = game;
    gGame              = &copy;
    gLog               = std::move(log);
    if (first >= kMaxSlots) return false;
    slots = std::min(slots, kMaxSlots - first);
    for (size_t i = 0; i < slots; ++i) {
        size_t t                  = first + i;
        gNames[t]                 = name;
        gSlots[t]                 = i;
        splitscreen_voriginals[t] = vtable[i];
        if (!writeSlot(&vtable[i], splitscreen_vtrampolines[t])) {
            gLog(fmt::format("trace: ERROR: could not write {} slot {}", name, i));
            for (size_t k = 0; k < i; ++k) writeSlot(&vtable[k], splitscreen_voriginals[first + k]);
            return false;
        }
    }
    gLog(fmt::format("trace: watching {} slots of {}", slots, name));
    return true;
}

void startTracing(int seconds) {
    if (gTracing.exchange(true)) return;
    for (auto& c : gCounts) c = 0;
    for (auto& s : gSeen) s = false;
    gLog(fmt::format("trace: calls for the next {} seconds", seconds));
    std::thread([seconds] {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        gTracing = false;
        std::string summary;
        for (size_t i = 0; i < kMaxSlots; ++i)
            if (gCounts[i]) summary += fmt::format(" {}:{}x{}", gNames[i], gSlots[i], gCounts[i].load());
        gLog(fmt::format("trace: calls (class:slot x count):{}", summary));
    }).detach();
}

} // namespace splitscreen

extern "C" void splitscreen_on_vcall(uint64_t slot, uint64_t* regs, uint64_t ret) {
    using namespace splitscreen;
    if (!gTracing.load(std::memory_order_relaxed)) return;
    ++gCounts[slot];
    if (gSeen[slot].exchange(true)) return;
    gLog(fmt::format("trace: {} virtual {} called from {} (x1=0x{:X} w1={} w2={})", gNames[slot], gSlots[slot],
                     offset(ret), regs[1], int32_t(regs[1]), int32_t(regs[2])));
}
