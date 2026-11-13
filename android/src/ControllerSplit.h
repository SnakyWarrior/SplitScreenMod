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

#include <chrono>
#include <functional>
#include <vector>

namespace splitscreen {

// managerAdded/managerRemoved: the manager's add/remove device functions (from the JNI functions' code).
bool startControllerSplit(Module const& game, uintptr_t managerObject, uintptr_t managerAdded, uintptr_t managerRemoved,
                          LogFn log);

// false (default): player 1 keeps touch and every controller joins as a new player with Start (controllers 1-3).
// true: the first controller plays player 1, as the game alone does.
void setFirstControllerIsPlayer1(bool yes);

// Called on the input thread, after the game handled it, when a controller other than the first presses Start.
void onStartPressed(std::function<void(int controller)> handler);

// Called on the input thread, after the game handled it, when a controller other than the first presses Select
// (View / Back / Share).
void onSelectPressed(std::function<void(int controller)> handler);

// The controller numbers that play (players 2-4 by controller number): a new controller takes over one of them
// whose controller is gone before it gets a free number.
void setPlayingPlayers(std::function<std::vector<int>()> playing);

// Called on the input thread when a new controller took over player `controller`, who had none.
void onLostPlayerTakenOver(std::function<void(int controller)> handler);

// Whether a connected device plays as controller number `controller`.
bool controllerConnected(int controller);

// While `blocked`, the controller's buttons and sticks reach nobody (its player is leaving).
void blockController(int controller, bool blocked);

// The device of controller number a now plays as b, and b's (if any) as a.
void swapControllers(int a, int b);

// Runs the tasks that are due (on the input thread, after each input event: also those of a blocked controller).
void runDueTasks();

// Runs `task` on the input thread (the game's main thread) at the first input event after `delay`.
void runOnInputThread(std::function<void()> task, std::chrono::milliseconds delay = std::chrono::milliseconds(0));

} // namespace splitscreen
