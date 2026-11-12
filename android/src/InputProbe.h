#pragma once

// Finding how the game tells controllers apart on Android.
//
// The game reads controller input straight from Android's native input queue (the JNI functions only cover the
// keyboard and device add/remove). Each event carries an Android device id, read with AInputEvent_getDeviceId.
// Its entry in the game library's GOT is swapped for a wrapper that logs, per device, what kind of device it is,
// its gamepad buttons, and where in the game the id was asked for, with the code around it.

#include "Elf.h"

#include <functional>
#include <string>

namespace splitscreen {

using LogFn = std::function<void(std::string const&)>;

void startInputProbe(Module const& game, LogFn log);

// Writes `size` bytes of code at `address` as hex lines that llvm-mc can disassemble.
void dumpCode(Module const& game, char const* label, uintptr_t address, size_t size, LogFn const& log);

} // namespace splitscreen
