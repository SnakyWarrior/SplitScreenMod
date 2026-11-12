#pragma once

// Traces which virtual functions of one class are called while tracing is on (VTrampolines.S): every slot of the
// vtable is replaced by a register-saving detour that logs the slot the first time it is called (and how often
// it was called when tracing ends), then jumps to the original function.

#include "Elf.h"
#include "InputProbe.h"

namespace splitscreen {

bool traceVtable(Module const& game, uintptr_t* vtable, size_t slots, char const* name, LogFn log);

// Logs calls from now on for `seconds`, then a summary.
void startTracing(int seconds);

} // namespace splitscreen
