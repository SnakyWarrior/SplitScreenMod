#include "mod/Overlay.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <thread>

#include <Windows.h>

#include "ll/api/thread/ClientThreadExecutor.h"

namespace overlay {
namespace {

struct Menu {
    std::wstring              title, text;
    std::vector<std::wstring> buttons;
    std::function<void(int)>  done;
    std::vector<Icon>         icons;
    int                       selected = 0;
};

std::atomic_bool gOpen = false;

std::wstring widen(std::string const& s) {
    int          n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

HWND gameWindow() {
    struct Find {
        HWND best = nullptr;
        long area = 0;
    } find;
    EnumWindows(
        [](HWND w, LPARAM p) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            RECT r{};
            if (pid == GetCurrentProcessId() && IsWindowVisible(w) && !GetWindow(w, GW_OWNER) && GetClientRect(w, &r)) {
                auto f    = reinterpret_cast<Find*>(p);
                long area = (r.right - r.left) * (r.bottom - r.top);
                if (area > f->area) f->best = w, f->area = area;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&find)
    );
    return find.best;
}

// The game's own font, so the menu looks like Minecraft.
std::wstring fontFace() {
    static std::wstring face = [] {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        auto font = std::filesystem::path(exe).parent_path() / "data" / "fonts" / "Mojangles.ttf";
        return AddFontResourceExW(font.c_str(), FR_PRIVATE, nullptr) > 0 ? std::wstring(L"Mojangles") : std::wstring(L"Segoe UI");
    }();
    return face;
}

// Layout in "game pixels" scaled to the window, like the game's GUI scale.
struct Layout {
    int  unit, width, height, titleH, textH, buttonH, gap, pad;
    RECT button(int i) const {
        int top = titleH + textH + pad + i * (buttonH + gap);
        return {pad, top, width - pad, top + buttonH};
    }
};

Layout layoutFor(Menu const& m, RECT const& game) {
    int    gameH = std::max(1L, game.bottom - game.top);
    Layout l{};
    l.unit    = std::max(1, gameH / 360);
    l.pad     = 8 * l.unit;
    l.gap     = 3 * l.unit;
    l.titleH  = 16 * l.unit;
    l.textH   = 18 * l.unit;
    l.buttonH = 22 * l.unit;
    l.width   = 220 * l.unit;
    l.height  = l.titleH + l.textH + l.pad * 2 + int(m.buttons.size()) * (l.buttonH + l.gap);
    return l;
}

void fill(HDC dc, RECT r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void frame(HDC dc, RECT r, COLORREF c, int t) {
    fill(dc, {r.left, r.top, r.right, r.top + t}, c);
    fill(dc, {r.left, r.bottom - t, r.right, r.bottom}, c);
    fill(dc, {r.left, r.top, r.left + t, r.bottom}, c);
    fill(dc, {r.right - t, r.top, r.right, r.bottom}, c);
}

// An 8x8 face, drawn pixel by pixel with a dark outline.
void drawIcon(HDC dc, Icon const& icon, int left, int top, int size, int unit) {
    if (icon.size() != 64) return;
    fill(dc, {left - unit, top - unit, left + size + unit, top + size + unit}, RGB(30, 30, 31));
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            fill(dc, {left + x * size / 8, top + y * size / 8, left + (x + 1) * size / 8, top + (y + 1) * size / 8}, icon[y * 8 + x]);
}

void paint(HDC dc, Menu const& m, Layout const& l) {
    // Bedrock style: light grey panel with a dark outline, grey buttons, the selected one green.
    RECT all{0, 0, l.width, l.height};
    fill(dc, all, RGB(198, 198, 198));
    frame(dc, all, RGB(30, 30, 31), l.unit);
    fill(dc, {l.unit, l.titleH, l.width - l.unit, l.height - l.unit}, RGB(49, 50, 51));
    HFONT big   = CreateFontW(-10 * l.unit, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, NONANTIALIASED_QUALITY, 0, fontFace().c_str());
    HFONT smallFont = CreateFontW(-8 * l.unit, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, NONANTIALIASED_QUALITY, 0, fontFace().c_str());
    SetBkMode(dc, TRANSPARENT);
    auto old = SelectObject(dc, big);
    SetTextColor(dc, RGB(49, 50, 51));
    RECT title{0, 0, l.width, l.titleH};
    DrawTextW(dc, m.title.c_str(), -1, &title, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, smallFont);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT text{l.pad, l.titleH, l.width - l.pad, l.titleH + l.textH + l.pad};
    DrawTextW(dc, m.text.c_str(), -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    for (int i = 0; i < int(m.buttons.size()); ++i) {
        RECT r   = l.button(i);
        bool sel = i == m.selected;
        fill(dc, r, sel ? RGB(60, 133, 39) : RGB(208, 209, 212));
        fill(dc, {r.left, r.bottom - 2 * l.unit, r.right, r.bottom}, sel ? RGB(29, 77, 19) : RGB(88, 88, 90));
        frame(dc, r, sel ? RGB(255, 255, 255) : RGB(30, 30, 31), l.unit);
        SetTextColor(dc, sel ? RGB(255, 255, 255) : RGB(49, 50, 51));
        RECT label = r;
        label.bottom -= 2 * l.unit;
        auto lines = m.buttons[i];
        UINT align = DT_CENTER;
        if (!m.icons.empty()) { // faces on the left, names next to them
            int size = 14 * l.unit;
            if (i < int(m.icons.size()))
                drawIcon(dc, m.icons[i], r.left + 5 * l.unit, r.top + (label.bottom - r.top - size) / 2, size, l.unit);
            label.left += 26 * l.unit;
            align = DT_LEFT;
        }
        DrawTextW(dc, lines.c_str(), -1, &label, align | DT_VCENTER | (lines.find(L'\n') == std::wstring::npos ? DT_SINGLELINE : 0));
    }
    SelectObject(dc, old);
    DeleteObject(big);
    DeleteObject(smallFont);
}

// XInput is loaded at run time so the mod does not need it at link time.
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, void*);
struct PadState {
    DWORD packet;
    WORD  buttons;
    BYTE  lt, rt;
    SHORT lx, ly, rx, ry;
};

XInputGetStateFn xinput() {
    static auto fn = [] {
        HMODULE dll = LoadLibraryW(L"xinput1_4.dll");
        return dll ? reinterpret_cast<XInputGetStateFn>(GetProcAddress(dll, "XInputGetState")) : nullptr;
    }();
    return fn;
}

enum class Key { None, Up, Down, Accept, Cancel };

// While a menu is open its keys are taken before the game sees them; otherwise Enter would also open the chat and Esc
// the pause menu.
std::atomic<int> gHookKey  = 0;
HWND             gHookGame = nullptr;
HHOOK            gHook     = nullptr; // menu thread only

LRESULT CALLBACK keyHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && GetForegroundWindow() == gHookGame) {
        Key key;
        switch (reinterpret_cast<KBDLLHOOKSTRUCT*>(lp)->vkCode) {
        case VK_UP: key = Key::Up; break;
        case VK_DOWN: key = Key::Down; break;
        case VK_RETURN: key = Key::Accept; break;
        case VK_ESCAPE: key = Key::Cancel; break;
        default: key = Key::None;
        }
        if (key != Key::None) {
            if (wp == WM_KEYDOWN) gHookKey = int(key);
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// Edge-detected input from every controller and, while the game has focus, the keyboard.
Key poll(HWND game) {
    static WORD  last[4]{};
    static bool  keyWas[4]{};
    Key          key = Key::None;
    if (auto get = xinput())
        for (DWORD i = 0; i < 4; ++i) {
            PadState s{};
            if (get(i, &s) != ERROR_SUCCESS) {
                last[i] = 0;
                continue;
            }
            WORD now = s.buttons;
            if (s.ly > 16000) now |= 0x0001; // stick up counts as D-pad up
            if (s.ly < -16000) now |= 0x0002;
            WORD pressed = now & ~last[i];
            last[i]      = now;
            if (pressed & 0x0001) key = Key::Up;
            if (pressed & 0x0002) key = Key::Down;
            if (pressed & 0x1000) key = Key::Accept; // A
            if (pressed & 0x2000) key = Key::Cancel; // B
        }
    if (int hooked = gHookKey.exchange(0)) key = Key(hooked);
    if (!gHook && GetForegroundWindow() == game) {
        int  vks[4]{VK_UP, VK_DOWN, VK_RETURN, VK_ESCAPE};
        Key  map[4]{Key::Up, Key::Down, Key::Accept, Key::Cancel};
        for (int i = 0; i < 4; ++i) {
            bool down = GetAsyncKeyState(vks[i]) & 0x8000;
            if (down && !keyWas[i]) key = map[i];
            keyWas[i] = down;
        }
    }
    return key;
}

LRESULT CALLBACK windowProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    auto menu = reinterpret_cast<Menu*>(GetWindowLongPtrW(w, GWLP_USERDATA));
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE; // never take focus from the game
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC         dc = BeginPaint(w, &ps);
        if (menu) {
            RECT r{};
            GetClientRect(w, &r);
            HDC     mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, r.right, r.bottom);
            auto    old = SelectObject(mem, bmp);
            RECT    game{};
            GetClientRect(GetWindow(w, GW_OWNER), &game);
            paint(mem, *menu, layoutFor(*menu, game));
            BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(w, &ps);
        return 0;
    }
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
        if (menu) {
            RECT game{};
            GetClientRect(GetWindow(w, GW_OWNER), &game);
            auto  l = layoutFor(*menu, game);
            POINT p{short(LOWORD(lp)), short(HIWORD(lp))};
            for (int i = 0; i < int(menu->buttons.size()); ++i) {
                RECT r = l.button(i);
                if (PtInRect(&r, p)) {
                    if (menu->selected != i) menu->selected = i, InvalidateRect(w, nullptr, FALSE);
                    if (msg == WM_LBUTTONDOWN) PostMessageW(w, WM_APP, i, 0);
                }
            }
        }
        return 0;
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

void run(Menu menu) {
    HWND game = gameWindow();
    if (!game) {
        gOpen = false;
        ll::thread::ClientThreadExecutor::getDefault().execute([done = menu.done] { done(-1); });
        return;
    }
    static ATOM cls = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc   = windowProc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"SplitScreenModMenu";
        return RegisterClassW(&wc);
    }();
    (void)cls;
    HWND w = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"SplitScreenModMenu",
        L"",
        WS_POPUP,
        0,
        0,
        1,
        1,
        game, // owned by the game window: stays above it and minimises with it
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr
    );
    SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&menu));
    gHookGame = game;
    gHookKey  = 0;
    gHook     = SetWindowsHookExW(WH_KEYBOARD_LL, keyHook, GetModuleHandleW(nullptr), 0); // this loop pumps its messages
    int  result = -1;
    bool done   = false;
    RECT placed{};
    while (!done && IsWindow(game)) {
        // follow the game window, centred over its client area
        RECT client{};
        GetClientRect(game, &client);
        POINT origin{0, 0};
        ClientToScreen(game, &origin);
        auto l = layoutFor(menu, client);
        RECT want{origin.x + (client.right - l.width) / 2, origin.y + (client.bottom - l.height) / 2, 0, 0};
        want.right  = want.left + l.width;
        want.bottom = want.top + l.height;
        if (!EqualRect(&want, &placed)) {
            SetWindowPos(w, HWND_TOPMOST, want.left, want.top, l.width, l.height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            InvalidateRect(w, nullptr, FALSE);
            placed = want;
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_APP) result = int(msg.wParam), done = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        switch (poll(game)) {
        case Key::Up: menu.selected = (menu.selected + int(menu.buttons.size()) - 1) % int(menu.buttons.size()); break;
        case Key::Down: menu.selected = (menu.selected + 1) % int(menu.buttons.size()); break;
        case Key::Accept: result = menu.selected, done = true; continue;
        case Key::Cancel: result = -1, done = true; continue;
        default: Sleep(16); continue;
        }
        InvalidateRect(w, nullptr, FALSE);
    }
    if (gHook) UnhookWindowsHookEx(gHook), gHook = nullptr;
    DestroyWindow(w);
    gOpen = false;
    ll::thread::ClientThreadExecutor::getDefault().execute([done = menu.done, result] { done(result); });
}

} // namespace

void show(std::string title, std::string text, std::vector<std::string> buttons, std::function<void(int)> done, std::vector<Icon> icons) {
    if (buttons.empty() || gOpen.exchange(true)) return;
    bool any = std::any_of(icons.begin(), icons.end(), [](Icon const& i) { return !i.empty(); });
    Menu menu{widen(title), widen(text), {}, std::move(done), any ? std::move(icons) : std::vector<Icon>{}};
    for (auto& b : buttons) menu.buttons.push_back(widen(b));
    poll(nullptr); // forget presses made before the menu opened
    std::thread(run, std::move(menu)).detach();
}

bool isOpen() { return gOpen; }

// ---- the pause-menu button ----
namespace {
std::atomic_bool      gButtonWanted = false, gButtonRunning = false, gHover = false;
std::mutex            gButtonLock;
std::function<void()> gButtonClick;
float                 gArea[4]{0, 0, 1, 1}; // under gButtonLock

int buttonUnit(HWND game) {
    RECT r{};
    GetClientRect(game, &r);
    return std::max(1L, r.bottom / 360);
}

LRESULT CALLBACK buttonProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_LBUTTONDOWN: PostMessageW(w, WM_APP, 0, 0); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC         dc = BeginPaint(w, &ps);
        RECT        r{};
        GetClientRect(w, &r);
        int  u   = buttonUnit(GetWindow(w, GW_OWNER));
        bool sel = gHover;
        fill(dc, r, sel ? RGB(60, 133, 39) : RGB(208, 209, 212));
        fill(dc, {r.left, r.bottom - 2 * u, r.right, r.bottom}, sel ? RGB(29, 77, 19) : RGB(88, 88, 90));
        frame(dc, r, sel ? RGB(255, 255, 255) : RGB(30, 30, 31), u);
        // two little screens side by side as the icon
        int      s = 12 * u, top = (r.bottom - 2 * u - s) / 2, left = 5 * u;
        COLORREF ink = sel ? RGB(255, 255, 255) : RGB(49, 50, 51);
        frame(dc, {left, top, left + s, top + s}, ink, u);
        fill(dc, {left + s / 2 - u / 2, top, left + s / 2 - u / 2 + u, top + s}, ink);
        HFONT font = CreateFontW(-8 * u, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, NONANTIALIASED_QUALITY, 0, fontFace().c_str());
        auto  old  = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ink);
        RECT label{left + s + 5 * u, 0, r.right - 5 * u, r.bottom - 2 * u};
        DrawTextW(dc, L"Split screen", -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextW(dc, L"(Y)", -1, &label, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old);
        DeleteObject(font);
        EndPaint(w, &ps);
        return 0;
    }
    }
    return DefWindowProcW(w, msg, wp, lp);
}

void buttonRun() {
    HWND game = gameWindow();
    static ATOM cls = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc   = buttonProc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.hCursor       = LoadCursor(nullptr, IDC_HAND);
        wc.lpszClassName = L"SplitScreenModButton";
        return RegisterClassW(&wc);
    }();
    (void)cls;
    HWND w = game ? CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"SplitScreenModButton", L"", WS_POPUP,
                                    0, 0, 1, 1, game, nullptr, GetModuleHandleW(nullptr), nullptr)
                  : nullptr;
    WORD last[4]{0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}; // a Y already held when the pause menu opened does not count
    RECT placed{};
    bool shown = false;
    while (w && gButtonWanted && IsWindow(game)) {
        bool clicked = false;
        MSG  msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_APP) clicked = true;
            DispatchMessageW(&msg);
        }
        bool visible = !gOpen && !IsIconic(game);
        if (auto get = xinput())
            for (DWORD i = 0; i < 4; ++i) {
                PadState s{};
                WORD     now = get(i, &s) == ERROR_SUCCESS ? s.buttons : 0;
                if (visible && (now & ~last[i] & 0x8000)) clicked = true; // Y
                last[i] = now;
            }
        int   u = buttonUnit(game);
        RECT  client{};
        GetClientRect(game, &client);
        POINT origin{0, 0};
        ClientToScreen(game, &origin);
        int  width = 110 * u, height = 22 * u;
        float area[4];
        {
            std::lock_guard lock(gButtonLock);
            std::copy(gArea, gArea + 4, area);
        }
        RECT want{origin.x + LONG((area[0] + area[2]) * client.right) - width - 8 * u, origin.y + LONG(area[1] * client.bottom) + 34 * u, 0, 0};
        want.right  = want.left + width;
        want.bottom = want.top + height;
        if (visible != shown || !EqualRect(&want, &placed)) {
            SetWindowPos(w, HWND_TOPMOST, want.left, want.top, width, height,
                         SWP_NOACTIVATE | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
            placed = want, shown = visible;
            InvalidateRect(w, nullptr, FALSE);
        }
        POINT cursor{};
        GetCursorPos(&cursor);
        if (bool hover = visible && PtInRect(&placed, cursor); hover != gHover) gHover = hover, InvalidateRect(w, nullptr, FALSE);
        if (clicked && visible) {
            std::lock_guard lock(gButtonLock);
            if (gButtonClick) ll::thread::ClientThreadExecutor::getDefault().execute(gButtonClick);
        }
        Sleep(16);
    }
    if (w) DestroyWindow(w);
    gButtonRunning = false;
}
} // namespace

void showButton(std::function<void()> click) {
    {
        std::lock_guard lock(gButtonLock);
        gButtonClick = std::move(click);
    }
    gButtonWanted = true;
    if (!gButtonRunning.exchange(true)) std::thread(buttonRun).detach();
}

void hideButton() { gButtonWanted = false; }

void moveButton(float x, float y, float width, float height) {
    std::lock_guard lock(gButtonLock);
    gArea[0] = x, gArea[1] = y, gArea[2] = width, gArea[3] = height;
}

} // namespace overlay
