#pragma once

// One player per controller on Android.
//
// Android's Minecraft keeps every controller apart in its controller manager (one slot per Android device), but
// tells its GameControllerManager that everything comes from controller 0: each call passes 0 in w1. So every
// controller moves player 1.
//
// Every controller event goes through one function of the controller manager, "dispatch":
//     dispatch(manager, ?, const std::variant<const Event*...>& event, bool)
// whose events start with the Android device id. While dispatch runs (and while the manager adds or removes a
// device), the device's controller number (0 for the first controller, 1 for the second, ...) is the "current
// controller" of that thread, and every GameControllerManager call made on that thread with controller 0 gets it
// instead. GameControllerManager's functions are found from the manager's code and dispatch's visitor table.

#include "Elf.h"
#include "InputProbe.h"

#include <functional>

namespace splitscreen {

// managerAdded/managerRemoved: the manager's add/remove device functions (from the JNI functions' code).
bool startControllerSplit(Module const& game, uintptr_t managerObject, uintptr_t managerAdded, uintptr_t managerRemoved,
                          LogFn log);

// false (default): player 1 keeps touch and every controller joins as a new player with Start (controllers 1-3).
// true: the first controller plays player 1, as the game alone does.
void setFirstControllerIsPlayer1(bool yes);

// Called on the input thread, after the game handled it, when a controller other than the first presses Start.
void onStartPressed(std::function<void(int controller)> handler);

} // namespace splitscreen
