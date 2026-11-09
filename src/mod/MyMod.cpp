#include "mod/MyMod.h"

#include <cstring>
#include <fstream>
#include <optional>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

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

// LeviLamina's getVtableIndex rejects 16-byte member pointers (classes with several bases, like AppPlatform)
// and knows one thunk form, so decode clang's vcall thunk here: mov rax,[rcx] then [rax+disp8/disp32].
static std::optional<uint> thunkSlot(uint8_t const* code) {
    for (int i = 0; i < 4 && code[0] == 0xE9; ++i) code += 5 + *reinterpret_cast<int32_t const*>(code + 1);
    if (code[0] != 0x48 || code[1] != 0x8B || code[2] != 0x01) return std::nullopt;
    auto op = code + 3;
    if (op[0] == 0x48) ++op; // mov rax,[rax+disp] instead of jmp [rax+disp]
    int32_t disp;
    if (op[1] == 0x60 || op[1] == 0x40) disp = static_cast<int8_t>(op[2]);
    else if (op[1] == 0xA0 || op[1] == 0x80) disp = *reinterpret_cast<int32_t const*>(op + 2);
    else if (op[1] == 0x20 || op[1] == 0x00) disp = 0;
    else return std::nullopt;
    if (disp < 0 || disp % 8) return std::nullopt;
    return static_cast<uint>(disp / 8);
}

template <class F>
static bool hookSlot(void* object, F method, ll::memory::FuncPtr detour, char const* name) {
    struct Raw {
        uint8_t const* code;
        int32_t        adjust; // only present in 16-byte member pointers
    } raw{};
    std::memcpy(&raw, &method, sizeof(method));
    auto index = thunkSlot(raw.code);
    if (!index) {
        note("ERROR: could not find the vtable slot for {} (thunk bytes {:02X})", name, fmt::join(raw.code, raw.code + 16, " "));
        return false;
    }
    auto self = static_cast<char*>(object) + (sizeof(method) > 8 ? raw.adjust : 0);
    Hooked h{(*reinterpret_cast<ll::memory::FuncPtr**>(self))[*index], detour, nullptr};
    if (ll::memory::hook(h.target, h.detour, &h.origin, ll::memory::HookPriority::Normal) != 0) {
        note("ERROR: hooking {} failed", name);
        return false;
    }
    gHooks.push_back(h);
    note("Hooked {} (vtable slot {}, this adjust {})", name, *index, sizeof(method) > 8 ? raw.adjust : 0);
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
