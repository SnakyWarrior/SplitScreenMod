#include "mod/MyMod.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/mod/RegisterHelper.h"
#include "mc/gamecore/pc/AppPlatform_GameCorePC.h"

namespace my_mod {

// The PC platform class overrides this to return false; force splitscreen on.
LL_TYPE_INSTANCE_HOOK(
    AllowSplitScreenHook,
    ll::memory::HookPriority::Normal,
    AppPlatform_GameCorePC,
    &AppPlatform_GameCorePC::$allowSplitScreen,
    bool
) {
    return true;
}

MyMod& MyMod::getInstance() {
    static MyMod instance;
    return instance;
}

bool MyMod::load() {
    getSelf().getLogger().debug("Loading...");
    // Code for loading the mod goes here.
    return true;
}

bool MyMod::enable() {
    ll::memory::HookRegistrar<AllowSplitScreenHook>::hook();
    getSelf().getLogger().info("Splitscreen forced on");
    // Code for enabling the mod goes here.
    return true;
}

bool MyMod::disable() {
    ll::memory::HookRegistrar<AllowSplitScreenHook>::unhook();
    // Code for disabling the mod goes here.
    return true;
}

} // namespace my_mod

LL_REGISTER_MOD(my_mod::MyMod, my_mod::MyMod::getInstance());
