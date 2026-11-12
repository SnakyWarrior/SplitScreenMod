#include "PostTrace.h"

#include <sys/uio.h>
#include <unistd.h>

#include <atomic>
#include <string>

#include <fmt/format.h>

namespace {
constexpr size_t kMax = 16;
}

extern "C" {
uintptr_t        splitscreen_poriginals[kMax];
extern uintptr_t splitscreen_ptrampolines[kMax];
}

namespace splitscreen {

namespace {

LogFn            gLog;
std::string      gNames[kMax];
size_t           gSlots[kMax];
size_t           gUsed = 0;
std::atomic<int> gRemaining{0};

bool safeRead(uintptr_t address, void* out, size_t size) {
    iovec local{out, size}, remote{reinterpret_cast<void*>(address), size};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == ssize_t(size);
}

} // namespace

bool watchResults(Module const& game, uintptr_t* vtable, std::vector<size_t> const& slots, char const* name, LogFn log) {
    gLog = std::move(log);
    for (size_t slot : slots) {
        if (gUsed >= kMax) return false;
        size_t i                  = gUsed++;
        gNames[i]                 = name;
        gSlots[i]                 = slot;
        splitscreen_poriginals[i] = vtable[slot];
        if (!writeSlot(&vtable[slot], splitscreen_ptrampolines[i])) {
            gLog(fmt::format("results: ERROR: could not write {} slot {}", name, slot));
            return false;
        }
        (void)game;
    }
    gLog(fmt::format("results: watching {} functions of {}", slots.size(), name));
    return true;
}

void logResults(int count) { gRemaining = count; }

} // namespace splitscreen

extern "C" void splitscreen_on_post(uint64_t index, uint64_t* regs, uint64_t x0, uint64_t x1, uint64_t d0) {
    using namespace splitscreen;
    if (gRemaining.load(std::memory_order_relaxed) <= 0) return;
    if (--gRemaining < 0) return;
    uint64_t sret[4] = {};
    bool     hasSret = regs[8] && safeRead(regs[8], sret, sizeof sret);
    uint64_t pointee[2] = {};
    bool     hasPointee = x0 > 0x10000 && safeRead(x0, pointee, sizeof pointee);
    gLog(fmt::format("results: {} virtual {}(w1={} w2={}) -> x0=0x{:X} x1=0x{:X} d0=0x{:X}{}{}", gNames[index],
                     gSlots[index], int32_t(regs[1]), int32_t(regs[2]), x0, x1, d0,
                     hasPointee ? fmt::format(" *x0=[0x{:X} 0x{:X}]", pointee[0], pointee[1]) : "",
                     hasSret ? fmt::format(" [x8]=[0x{:X} 0x{:X} 0x{:X} 0x{:X}]", sret[0], sret[1], sret[2], sret[3])
                             : ""));
}
