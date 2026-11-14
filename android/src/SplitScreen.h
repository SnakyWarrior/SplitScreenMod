#pragma once

#include <pl/Mod.hpp>

namespace splitscreen {

class SplitScreenMod {
public:
    static SplitScreenMod& instance();

    SplitScreenMod() : mSelf(*ll::mod::NativeMod::current()) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();

private:
    ll::mod::NativeMod& mSelf;
};

} // namespace splitscreen
