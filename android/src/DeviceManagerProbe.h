#pragma once

// The game's native controller bookkeeping on Android.
//
// Java's JellyBeanDeviceManager reports each controller to the game through JNI (onInputDeviceAddedNative,
// onInputDeviceRemovedNative, setControllerDetailsNative). Each of these ends in
//     adrp x0, object@page; add x0, x0, object@pageoff; mov w1, <device id>; bl <method>
// i.e. a call on one static object in the game: its controller manager. This finds that object and the methods
// from the code (no fixed addresses), logs its class name, the methods' code, and the object's memory whenever it
// changes, to see where devices 11 and 12 end up.

#include "Elf.h"
#include "InputProbe.h"

#include <vector>

namespace splitscreen {

struct DeviceManagerCall {
    char const* jni;    // e.g. "onInputDeviceAddedNative"
    uintptr_t   object; // the static object passed in x0
    uintptr_t   method; // the function called on it
};

std::vector<DeviceManagerCall> findDeviceManagerCalls(Module const& game, std::vector<Symbol> const& exports);

// The RTTI class name of a polymorphic object in the game, or "" if it has none.
std::string className(Module const& game, uintptr_t object);

// Logs the object's memory now and every 10 seconds when it changed (for an hour).
void watchObject(Module const& game, uintptr_t object, size_t size, LogFn log);

} // namespace splitscreen
