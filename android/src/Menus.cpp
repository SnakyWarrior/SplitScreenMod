#include "Menus.h"

#include "FormProbe.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>

#include <fmt/format.h>

#include <sys/system_properties.h>

namespace splitscreen {

void logLine(std::string const& line); // SplitScreen.cpp

namespace menus {

namespace {

template <class... Args>
void note(fmt::format_string<Args...> f, Args&&... args) {
    logLine(fmt::format(f, std::forward<Args>(args)...));
}

// --- texts: the resource pack's .lang files -------------------------------------------------------------------------
std::mutex                         gTextsMutex;
std::map<std::string, std::string> gTexts;

std::map<std::string, std::string> readLang(std::filesystem::path const& file) {
    std::map<std::string, std::string> out;
    std::ifstream                      in(file);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto eq = line.find('=');
        if (eq != std::string::npos && line.rfind("##", 0) != 0) out[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return out;
}

// `key` in the game's language, its "%s" replaced by `params` in turn.
std::string tr(std::string const& key, std::vector<std::string> const& params = {}) {
    std::string text;
    {
        std::lock_guard lock(gTextsMutex);
        auto            it = gTexts.find(key);
        text               = it != gTexts.end() ? it->second : key;
    }
    size_t at = 0;
    for (auto const& p : params) {
        at = text.find("%s", at);
        if (at == std::string::npos) break;
        text.replace(at, 2, p);
        at += p.size();
    }
    return text;
}

// --- the Windows version's helpers ----------------------------------------------------------------------------------
// Button texts the resource pack draws specially (see its server_form.json): a square button laid over the right end
// of the row above it (remove that player / take player 1's controller), a row that leaves room for that square, and
// the green closing button.
constexpr char const* kKickButton    = "[x]";
constexpr char const* kPadButton     = "[p]";
constexpr char const* kRoomForSquare = " "; // a leading space: not seen, but the UI can test for it
constexpr char const* kTitleMenu     = "[ss:m]"; // Split screen
constexpr char const* kTitleWho      = "[ss:w]"; // Who is playing?
constexpr char const* kTitleChoose   = "[ss:c]"; // Choose your player (black background, countdown)
// Player pictures: P1-P4 (the same as their part of the screen) for players with a controller, else the name tag.
constexpr char const* kNameTag = "textures/items/name_tag";
// "Choose your player" closes by itself after 10 seconds (the resource pack shows the countdown).
constexpr int kChooseSeconds = 10;
constexpr int kTimedOut      = -2;
constexpr auto kRemoveAfter  = std::chrono::seconds(20);

// White: the player colours of the Windows version come from the level, which the Android version cannot read yet.
std::string badge(int sub) { return fmt::format("textures/splitscreen/p{}_24", sub + 1); }

// "Player 2": the game's local player names are not read on Android yet.
std::string localName(int sub) {
    std::string name = tr("splitscreen.tag.player1");
    if (auto one = name.rfind('1'); one != std::string::npos) name.replace(one, 1, std::to_string(sub + 1));
    return name;
}

// A short line above that player's hotbar, so they see what a menu choice did. Not on Android yet (the game's
// jukebox popup packet is not built yet): logged only.
void tell(int sub, std::string const& text) { note("tell player {}: {}", sub + 1, text); }

void showMenu(int who, std::string const& text, std::vector<std::string> const& buttons,
              std::vector<std::string> const& pictures, std::function<void(int)> done, std::string const& title,
              int timeoutSeconds = 0) {
    if (buttons.empty() || menuOpen(who)) return note("menu for player {} not shown (already open)", who + 1);
    showForm(who, title, text, buttons, pictures, std::move(done), timeoutSeconds, kTimedOut);
}

std::atomic<int> gFreshSub{-1}; // a player that just joined and still has to be asked about

// When the players or their controllers change, every open Split screen menu is closed and opened again with what is
// true now (the Windows version's refreshSettings). `gSettingsOpen`: player -> when their menu opened.
std::mutex                                           gSettingsMutex;
std::map<int, std::chrono::steady_clock::time_point> gSettingsOpen, gReopenAt;
std::string                                          gLastState;
std::mutex       gMissingMutex;
std::map<int, std::chrono::steady_clock::time_point> gMissingSince; // sub -> when its controller went

// Show the menu on player `owner`'s screen. `fresh`: the player just joined (rather than picking up a controller).
void offerPicker(int owner, bool fresh) {
    auto players = game::players();
    if (!players.contains(owner)) return;
    int controller = players.at(owner);

    enum class Kind { Keep, TakeOver, NewPlayer };
    struct Choice {
        Kind kind;
        int  value; // controller (TakeOver)
        int  sub;
    };
    // One button per player in player order (player 1 first): this controller's own player and players without a
    // controller. (The Windows version also lists saved players who are not playing.)
    std::vector<Choice> choices{{Kind::Keep, 0, owner}};
    for (auto [sub, other] : players)
        if (sub != owner && !game::hasController(other)) choices.push_back({Kind::TakeOver, other, sub});
    std::stable_sort(choices.begin(), choices.end(), [](Choice const& a, Choice const& b) { return a.sub < b.sub; });
    std::vector<std::string> buttons;
    std::vector<std::string> pictures;
    std::vector<int>         toChoice; // form button -> choice; -1 for the status tags drawn on the row above
    for (int i = 0; i < int(choices.size()); ++i) {
        auto& c      = choices[i];
        bool  hasPad = c.kind == Kind::Keep && !fresh; // only this controller's own player has one in this list
        buttons.push_back(localName(c.sub));
        pictures.push_back(hasPad ? badge(c.sub) : kNameTag);
        toChoice.push_back(i);
        // the stand-in of a new controller is not really playing yet
        buttons.push_back(c.kind == Kind::Keep && fresh ? "[tag:Not playing]"
                          : c.kind == Kind::TakeOver    ? "[tag:No controller]"
                                                        : "[tag:Playing]");
        pictures.emplace_back();
        toChoice.push_back(-1);
    }
    if (!fresh && players.size() < 4) {
        toChoice.push_back(int(choices.size()));
        choices.push_back({Kind::NewPlayer, 0, owner});
        buttons.push_back(tr("splitscreen.newPlayer"));
        pictures.emplace_back();
    }
    int const keep = int(std::find_if(choices.begin(), choices.end(), [](Choice const& c) { return c.kind == Kind::Keep; })
                         - choices.begin());
    if (choices.size() < 2) {
        if (fresh) gFreshSub = -1; // nobody else to be: the new player is who joined
        return;
    }
    note("picker: menu for player {} ({} choices)", owner + 1, choices.size());
    showMenu(owner, tr("splitscreen.picker.body"), buttons, pictures, [=](int pick) {
        if (fresh) gFreshSub = -1;
        if (fresh && pick == kTimedOut) { // nobody chosen in 10 s: nobody joins, the stand-in leaves
            note("picker: nobody chosen for player {} in {} s", owner + 1, kChooseSeconds);
            return game::requestLeave(owner);
        }
        pick = pick >= 0 && pick < int(toChoice.size()) ? toChoice[pick] : -1;
        if (fresh && (pick == keep || pick < 0 || pick >= int(choices.size()))) // kept: the stand-in is the chosen player
            tell(owner, tr("splitscreen.tell.youAre", {localName(owner)}));
        if (pick == keep || pick < 0 || pick >= int(choices.size())) return; // closed, or kept
        auto& choice = choices[pick];
        switch (choice.kind) {
        case Kind::TakeOver:
            // (fresh: the player made for this controller is not needed any more)
            if (fresh) game::handOver(owner, controller, choice.value);
            else game::moveController(controller, choice.value);
            tell(choice.sub, tr("splitscreen.tell.youAreNow", {localName(choice.sub)}));
            break;
        case Kind::NewPlayer: game::join(controller); break; // this player stays and waits for a controller
        default: break;
        }
    }, fresh ? kTitleChoose : kTitleWho, fresh ? kChooseSeconds : 0);
}

} // namespace

void setTexts(std::filesystem::path const& pack, std::filesystem::path const& options) {
    std::string language = "en_US";
    std::ifstream in(options);
    for (std::string line; std::getline(in, line);)
        if (line.rfind("game_language:", 0) == 0) {
            language = line.substr(14);
            while (!language.empty() && (language.back() == '\r' || language.back() == ' ')) language.pop_back();
        }
    if (language.empty()) { // the game follows the phone's language: "el-GR" -> "el_GR"
        for (char const* prop : {"persist.sys.locale", "ro.product.locale", "persist.sys.language"}) {
            char value[PROP_VALUE_MAX] = {};
            __system_property_get(prop, value);
            if ((language = value).size() >= 2) break;
        }
        if (language.size() == 2) language += "_" + std::string(language == "el" ? "GR" : language == "en" ? "US" : "");
        std::replace(language.begin(), language.end(), '-', '_');
    }
    auto texts = readLang(pack / "texts" / "en_US.lang");
    auto local = readLang(pack / "texts" / (language + ".lang"));
    if (local.empty()) // e.g. "el_CY": the first of the pack's languages that starts the same
        for (auto const& f : std::filesystem::directory_iterator(pack / "texts", std::filesystem::directory_options::skip_permission_denied))
            if (f.path().filename().string().rfind(language.substr(0, 2), 0) == 0) {
                local = readLang(f.path());
                break;
            }
    for (auto& [k, v] : local) texts[k] = v;
    note("menus: texts in {} ({} lines)", language, texts.size());
    std::lock_guard lock(gTextsMutex);
    gTexts = std::move(texts);
}

// The Split screen menu: the players, to switch to or remove.
void onSettingsButton(int who) {
    auto players = game::players();
    if (!players.contains(who) || gFreshSub == who) return;
    if (menuOpen(who)) return;
    int  myPad = players.at(who);

    bool firstPad = game::hasController(0); // player 1's controller, if they have one (else they play on touch)
    enum class Act { Swap, Kick, FreePad, Done, Nothing };
    struct Item {
        Act act;
        int sub;
    };
    // The Windows version's menu also has the layout (top and bottom / side by side) and saved players who are not
    // playing (to switch to); not on Android yet.
    std::vector<Item>        items{{Act::Nothing, -1}};
    std::vector<std::string> buttons{"[h:Players]"};
    std::vector<std::string> pictures{""};
    for (auto [sub, controller] : players) {
        bool hasPad = game::hasController(controller);
        auto name   = localName(sub);
        auto status = sub == 0 ? "Player 1" : hasPad ? "Playing" : "No controller";
        // a square button at the end of the row: remove players 2-4, or take player 1's controller if they have one
        bool square = sub != 0 || firstPad;
        items.push_back({Act::Swap, sub});
        buttons.push_back(fmt::format("{}{}", square ? kRoomForSquare : "", name));
        pictures.push_back(hasPad ? badge(sub) : kNameTag);
        if (sub == who) { // "You", drawn on the row above
            items.push_back({Act::Nothing, -1});
            buttons.push_back(square ? "[syou]" : "[you]");
            pictures.emplace_back();
        }
        items.push_back({Act::Nothing, -1}); // the status tag, drawn on the row above
        buttons.push_back(fmt::format("[{}:{}]", square ? "stag" : "tag", status));
        pictures.emplace_back();
        if (square) {
            items.push_back({sub == 0 ? Act::FreePad : Act::Kick, sub});
            buttons.push_back(sub == 0 ? kPadButton : kKickButton);
            pictures.push_back(sub == 0 ? "textures/ui/controller_glyph" : "textures/splitscreen/cross");
        }
    }
    items.push_back({Act::Done, -1});
    buttons.push_back("[d]");
    pictures.emplace_back();

    note("menus: player {} opens the Split screen menu", who + 1);
    {
        std::lock_guard lock(gSettingsMutex);
        gSettingsOpen[who] = std::chrono::steady_clock::now();
    }
    showMenu(who, "", buttons, pictures, [items, who, myPad](int pick) {
        {
            std::lock_guard lock(gSettingsMutex);
            gSettingsOpen.erase(who);
        }
        if (pick < 0 || pick >= int(items.size())) return;
        auto players = game::players();
        auto item    = items[pick];
        switch (item.act) {
        case Act::Swap: { // this controller plays as that player; theirs (if any) comes here
            if (item.sub == who || !players.contains(item.sub) || !players.contains(who)) return;
            bool iHavePad    = game::hasController(myPad);
            bool theyHavePad = game::hasController(players.at(item.sub));
            if (!iHavePad) return tell(who, tr("splitscreen.tell.needController"));
            // nothing comes back to this player (the other one had no controller): it leaves instead of waiting
            if (!theyHavePad && who != 0) game::handOver(who, myPad, players.at(item.sub));
            else game::moveController(myPad, players.at(item.sub));
            tell(item.sub, tr("splitscreen.tell.youAreNow", {localName(item.sub)}));
            return;
        }
        case Act::FreePad: // player 1 goes back to the touch screen; the controller waits for start to be pressed
            game::freeFirstController();
            tell(0, tr("splitscreen.tell.pressStart"));
            return;
        case Act::Kick:
            if (players.contains(item.sub)) {
                note("settings: player {} removed", item.sub + 1);
                tell(who, tr("splitscreen.tell.left", {localName(item.sub)}));
                game::requestLeave(item.sub);
            }
            return;
        default: return;
        }
    }, kTitleMenu);
}

// A new controller joins at once as the first free player, and its own part of the screen (black, see the resource
// pack's server_form.json) asks who is playing; picking someone else swaps that player in.
void onJoinRequest(int controller) {
    if (!inWorld()) return note("controller {} asked to join outside a world (or while it loads): not joining", controller);
    if (gFreshSub >= 0) return; // someone is joining already
    auto players = game::players();
    int  sub     = 1;
    while (sub <= 3 && players.contains(sub)) ++sub;
    if (sub > 3) return;
    gFreshSub = sub;
    note("picker: controller {} joins as player {}", controller, sub + 1);
    game::join(controller, [](int joined) {
        if (joined < 0) gFreshSub = -1;
        else offerPicker(joined, true); // shown once the new player is in the world
    });
}

void onControllerConnected(int sub) {
    if (sub != 0) offerPicker(sub, false);
}

void onPlayerLeft(int sub) {
    cancelMenu(sub);
    {
        std::lock_guard lock(gSettingsMutex);
        gSettingsOpen.erase(sub);
        gReopenAt.erase(sub);
    }
    int fresh = sub;
    gFreshSub.compare_exchange_strong(fresh, -1);
    std::lock_guard lock(gMissingMutex);
    gMissingSince.erase(sub);
}

void checkControllers() {
    auto now = std::chrono::steady_clock::now();
    // refresh the open Split screen menus when who plays (with or without a controller) changed
    {
        std::string state;
        for (auto [sub, controller] : game::players()) state += fmt::format("{}:{} ", sub, game::hasController(controller));
        std::vector<int> reopen;
        {
            std::lock_guard lock(gSettingsMutex);
            bool changed = !gLastState.empty() && state != gLastState;
            gLastState   = state;
            if (changed)
                for (auto it = gSettingsOpen.begin(); it != gSettingsOpen.end();) {
                    if (now - it->second < std::chrono::seconds(1)) { // already up to date
                        ++it;
                        continue;
                    }
                    note("settings: refreshing player {}'s menu", it->first + 1);
                    closeMenu(it->first);
                    gReopenAt[it->first] = now + std::chrono::milliseconds(800);
                    it                   = gSettingsOpen.erase(it);
                }
            for (auto it = gReopenAt.begin(); it != gReopenAt.end();)
                if (it->second <= now) {
                    reopen.push_back(it->first);
                    it = gReopenAt.erase(it);
                } else
                    ++it;
        }
        for (int sub : reopen) onSettingsButton(sub);
    }
    for (auto [sub, controller] : game::players()) {
        if (sub == 0 || sub == gFreshSub) continue;
        bool            has = game::hasController(controller);
        std::lock_guard lock(gMissingMutex);
        if (has) {
            gMissingSince.erase(sub);
            continue;
        }
        auto [lost, added] = gMissingSince.try_emplace(sub, now);
        if (added) note("player {}: controller gone", sub + 1);
        if (now - lost->second > kRemoveAfter) {
            note("player {}: no controller for {} s, removed", sub + 1, kRemoveAfter.count());
            gMissingSince.erase(lost);
            game::requestLeave(sub); // what the player's own "Save & Leave" does
        }
    }
}

} // namespace menus

} // namespace splitscreen
