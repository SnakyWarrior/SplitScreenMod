#include "mod/MyMod.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/memory/Memory.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/TargetedBedrock.h"
#include "mc/deps/application/AppPlatform.h"

namespace my_mod {

// LeviLamina exports no address for AppPlatform_GameCorePC::allowSplitScreen in this game build,
// so we read the real one from the live platform object's vtable and hook that.
static ll::memory::FuncPtr gTarget  = nullptr;
static ll::memory::FuncPtr gOrigin  = nullptr;

static bool allowSplitScreenDetour(AppPlatform*) { return true; }

MyMod& MyMod::getInstance() {
    static MyMod instance;
    return instance;
}

bool MyMod::load() { return true; }

bool MyMod::enable() {
    auto& logger   = getSelf().getLogger();
    auto  platform = ll::service::getAppPlatform();
    if (!platform) {
        logger.error("AppPlatform not available yet; splitscreen not enabled");
        return false;
    }
    auto index = ll::memory::getVtableIndex(&AppPlatform::allowSplitScreen);
    if (!index) {
        logger.error("Could not find allowSplitScreen vtable slot");
        return false;
    }
    auto vtable = *reinterpret_cast<ll::memory::FuncPtr**>(platform.as_ptr());
    gTarget     = vtable[*index];
    if (ll::memory::hook(
            gTarget,
            reinterpret_cast<ll::memory::FuncPtr>(&allowSplitScreenDetour),
            &gOrigin,
            ll::memory::HookPriority::Normal
        )
        != 0) {
        logger.error("Hooking allowSplitScreen failed");
        return false;
    }
    logger.info("Splitscreen forced on (vtable slot {})", *index);
    return true;
}

bool MyMod::disable() {
    if (gTarget) ll::memory::unhook(gTarget, reinterpret_cast<ll::memory::FuncPtr>(&allowSplitScreenDetour));
    gTarget = nullptr;
    return true;
}

} // namespace my_mod

LL_REGISTER_MOD(my_mod::MyMod, my_mod::MyMod::getInstance());
