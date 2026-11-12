#pragma once

// Traces which virtual functions of one class are called while tracing is on (VTrampolines.S): every slot of the
// vtable is replaced by a register-saving detour that logs the slot the first time it is called (and how often
// it was called when tracing ends), then jumps to the original function.

#include "Elf.h"
#include "InputProbe.h"

namespace splitscreen {

// Each traced class uses its own range of trampolines, starting at `first` (400 in all).
bool traceVtable(Module const& game, uintptr_t* vtable, size_t slots, size_t first, char const* name, LogFn log);

// Logs calls from now on for `seconds`, then a summary.
void startTracing(int seconds);

} // namespace splitscreen
