#include "mod/MyMod.h"

#include <fstream>
#include <vector>

#include <fmt/format.h>

#include "ll/api/memory/Hook.h"
#include "ll/api/memory/Memory.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/TargetedBedrock.h"
#include "mc/client/game/ClientInstance.h"
#include "mc/client/game/IMinecraftGame.h"
#include "mc/deps/application/AppPlatform.h"

namespace my_mod {

// LeviLamina exports no addresses for these PC overrides in this game build,
// so each one is read from the live object's vtable and hooked there.
struct Hooked {
    ll::memory::FuncPtr target, detour, origin;
};
static std::vector<Hooked> gHooks;

// LeviLamina's own log file is not written in this setup, so keep our own next to the mod.
template <class... Args>
static void note(fmt::format_string<Args...> f, Args&&... args) {
    auto        line = fmt::format(f, std::forward<Args>(args)...);
    auto&       self = MyMod::getInstance().getSelf();
    static auto out  = std::ofstream(self.getModDir() / "splitscreen.log", std::ios::trunc);
    self.getLogger().info("{}", line);
    out << line << std::endl;
}

static bool returnTrue(void*) { return true; }

static bool joinEnabledDetour(void*) {
    static bool logged = false;
    if (!logged) note("isSplitscreenJoinEnabled called by the game");
    logged = true;
    return true;
}

static bool canAddSubClientDetour(void*, int controllerId) {
    note("Splitscreen join requested by controller {}", controllerId);
    return true;
}

template <class F>
static bool hookSlot(void* object, F method, ll::memory::FuncPtr detour, char const* name) {
    auto index = ll::memory::getVtableIndex(method);
    if (!index) {
        note("ERROR: could not find the vtable slot for {}", name);
        return false;
    }
    Hooked h{(*static_cast<ll::memory::FuncPtr**>(object))[*index], detour, nullptr};
    if (ll::memory::hook(h.target, h.detour, &h.origin, ll::memory::HookPriority::Normal) != 0) {
        note("ERROR: hooking {} failed", name);
        return false;
    }
    gHooks.push_back(h);
    note("Hooked {} (vtable slot {})", name, *index);
    return true;
}

MyMod& MyMod::getInstance() {
    static MyMod instance;
    return instance;
}

bool MyMod::load() { return true; }

bool MyMod::enable() {
    note("SplitScreenMod enabling");
    auto  platform = ll::service::getAppPlatform();
    auto  client   = ll::service::getClientInstance();
    if (!platform || !client) {
        note("ERROR: game not ready (platform: {}, client: {})", bool(platform), bool(client));
        return false;
    }
    IMinecraftGame& game = client->getMinecraftGame();
    auto            fn   = [](auto f) { return reinterpret_cast<ll::memory::FuncPtr>(f); };
    bool            ok   = hookSlot(platform.as_ptr(), &AppPlatform::allowSplitScreen, fn(&returnTrue), "allowSplitScreen")
              && hookSlot(&game, &IMinecraftGame::isSplitscreenJoinEnabled, fn(&joinEnabledDetour), "isSplitscreenJoinEnabled")
              && hookSlot(
                  &game,
                  &IMinecraftGame::canAddSplitscreenSubClient,
                  fn(&canAddSubClientDetour),
                  "canAddSplitscreenSubClient"
              );
    if (!ok) disable();
    return ok;
}

bool MyMod::disable() {
    for (auto& h : gHooks) ll::memory::unhook(h.target, h.detour);
    gHooks.clear();
    return true;
}

} // namespace my_mod

LL_REGISTER_MOD(my_mod::MyMod, my_mod::MyMod::getInstance());
