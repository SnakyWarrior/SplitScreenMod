#include "VTrace.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include <fmt/format.h>

namespace {
constexpr size_t kMaxSlots = 320;
}

extern "C" {
uintptr_t       splitscreen_voriginals[kMaxSlots];
extern uintptr_t splitscreen_vtrampolines[kMaxSlots];
}

namespace splitscreen {

namespace {

Module const*         gGame = nullptr;
LogFn                 gLog;
std::string           gName;
std::atomic<bool>     gTracing{false};
std::atomic<uint64_t> gCounts[kMaxSlots];
std::atomic<bool>     gSeen[kMaxSlots];

std::string offset(uintptr_t p) {
    return gGame && gGame->isCode(p) ? fmt::format("+0x{:X}", p - gGame->base) : fmt::format("0x{:X}", p);
}

} // namespace

bool traceVtable(Module const& game, uintptr_t* vtable, size_t slots, char const* name, LogFn log) {
    static Module copy = game;
    gGame              = &copy;
    gLog               = std::move(log);
    gName              = name;
    slots              = std::min(slots, kMaxSlots);
    for (size_t i = 0; i < slots; ++i) {
        splitscreen_voriginals[i] = vtable[i];
        if (!writeSlot(&vtable[i], splitscreen_vtrampolines[i])) {
            gLog(fmt::format("trace: ERROR: could not write {} slot {}", name, i));
            for (size_t k = 0; k < i; ++k) writeSlot(&vtable[k], splitscreen_voriginals[k]);
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
    gLog(fmt::format("trace: {} calls for the next {} seconds", gName, seconds));
    std::thread([seconds] {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        gTracing = false;
        std::string summary;
        for (size_t i = 0; i < kMaxSlots; ++i)
            if (gCounts[i]) summary += fmt::format(" {}x{}", i, gCounts[i].load());
        gLog(fmt::format("trace: {} calls (slot x count):{}", gName, summary));
    }).detach();
}

} // namespace splitscreen

extern "C" void splitscreen_on_vcall(uint64_t slot, uint64_t* regs, uint64_t ret) {
    using namespace splitscreen;
    if (!gTracing.load(std::memory_order_relaxed)) return;
    ++gCounts[slot];
    if (gSeen[slot].exchange(true)) return;
    gLog(fmt::format("trace: {} virtual {} called from {} (x1=0x{:X} w1={} w2={})", gName, slot, offset(ret), regs[1],
                     int32_t(regs[1]), int32_t(regs[2])));
}
