#pragma once

// Logs what some virtual functions return (PostTrampolines.S): their x0/x1 result and, for functions that return
// a struct through x8 (e.g. a shared_ptr), the first words at x8. Only while result logging is on.

#include "Elf.h"
#include "InputProbe.h"

#include <vector>

namespace splitscreen {

bool watchResults(Module const& game, uintptr_t* vtable, std::vector<size_t> const& slots, char const* name, LogFn log);

// Logs up to `count` results from now on.
void logResults(int count);

} // namespace splitscreen
