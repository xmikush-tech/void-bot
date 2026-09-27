// ============================================================================
//  main.cpp — VOIDLoader  (Win32 GDI, zero external deps)
//  Layout: left PIN panel | right loader panel  (split at centre)
//  PIN: 6 boxes, keyboard input, auto-submit, shake on wrong
//  Build: gdi32.lib  user32.lib  dwmapi.lib   /SUBSYSTEM:WINDOWS
// ============================================================================
#define NOMINMAX
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <string>
#include <algorithm>
#include <cmath>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

// ── Config ────────────────────────────────────────────────────────────────────
namespace cfg {
    constexpr wchar_t kClass[]      = L"VOIDLoaderWnd";
    constexpr wchar_t kCorrectPin[] = L"123456";
    constexpr int     kW            = 850;
    constexpr int     kH            = 520;
    constexpr int     kBarH         = 52;     // top chrome height
    constexpr int     kPIN          = 6;
    constexpr const wchar_t* kExe   = L"void.exe";
}

// ── GDI colour helpers ────────────────────────────────────────────────────────
static inline COLORREF rgb(int r, int g, int b) { return RGB(r, g, b); }

// ── App state ─────────────────────────────────────────────────────────────────
namespace st {
    std::wstring pin;          // digits so far (max 6)
    bool   authed    = false;
    bool   error     = false;
    bool   launched  = false;

    // Shake
    bool   shaking   = false;
    DWORD  shakeStart= 0;
    static constexpr DWORD kShakeDur = 360; // ms

    // Loader
    int    progress  = 0;
    UINT_PTR timerId = 1;

    // Drag
    bool   dragging  = false;
    POINT  dragOrig  = {};
    POINT  wndOrig   = {};
}

// ── GDI helpers ───────────────────────────────────────────────────────────────
static HFONT MakeFont(int size, int weight, const wchar_t* face = L"Segoe UI") {
    return CreateFontW(size, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

static void GdiRect(HDC dc, RECT r, COLORREF fill, COLORREF border = 0, int rad = 0) {
    HBRUSH br = CreateSolidBrush(fill);
    HPEN   pn = CreatePen(PS_SOLID, 1, border ? border : fill);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, pn);
    if (rad > 0) RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    else         Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pn);
}

static void GdiText(HDC dc, const wchar_t* txt, RECT r, HFONT font,
                    COLORREF col, UINT flags = DT_CENTER | DT_VCENTER | DT_SINGLELINE) {
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, col);
    DrawTextW(dc, txt, -1, &r, flags);
    SelectObject(dc, old);
}

static void GdiLine(HDC dc, int x1, int y1, int x2, int y2, COLORREF col, int w = 1) {
    HPEN pn = CreatePen(PS_SOLID, w, col);
    HGDIOBJ op = SelectObject(dc, pn);
    MoveToEx(dc, x1, y1, nullptr);
    LineTo(dc, x2, y2);
    SelectObject(dc, op);
    DeleteObject(pn);
}

static void GdiCircle(HDC dc, int cx, int cy, int r, COLORREF fill, COLORREF border = 0) {
    HBRUSH br = CreateSolidBrush(fill);
    HPEN   pn = border ? CreatePen(PS_SOLID, 1, border) : (HPEN)GetStockObject(NULL_PEN);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, pn);
    Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    if (border) DeleteObject(pn);
}

// ── Shake offset (px) at current time ────────────────────────────────────────
static float ShakeX() {
    if (!st::shaking) return 0.f;
    DWORD elapsed = GetTickCount() - st::shakeStart;
    if (elapsed >= st::kShakeDur) { st::shaking = false; return 0.f; }
    float t  = 1.f - (float)elapsed / st::kShakeDur;            // 1→0
    float s  = sinf((float)elapsed * 0.042f) * 8.f * t;         // decaying sine
    return s;
}

// ── Launch cheat exe ──────────────────────────────────────────────────────────
static void LaunchExe(HWND hwnd) {
    if (st::launched) return;
    st::launched = true;
    wchar_t dir[MAX_PATH]; GetModuleFileNameW(nullptr, dir, MAX_PATH);
    // strip filename manually
    {
        wchar_t* p = wcsrchr(dir, L'\\');
        if (p) *p = L'\0';
    }
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.lpVerb      = L"runas";
    sei.lpFile      = cfg::kExe;
    sei.lpDirectory = dir;
    sei.nShow       = SW_HIDE;
    ShellExecuteExW(&sei);
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
}

// ── DrawUI (GDI, double-buffered via WM_PAINT) ────────────────────────────────
static void DrawUI(HDC dc, HWND hwnd) {
    RECT cl{}; GetClientRect(hwnd, &cl);
    const int W = cl.right, H = cl.bottom;
    const int cx = W / 2;    // centre divider X

    // ── Background ────────────────────────────────────────────────────────────
    {
        HBRUSH bg = CreateSolidBrush(rgb(10, 10, 15));
        FillRect(dc, &cl, bg);
        DeleteObject(bg);
    }

    // Subtle diagonal grid lines (decorative)
    for (int i = 0; i < 9; ++i)
        GdiLine(dc, 50 + i * 100, 60, 200 + i * 90, H - 40, rgb(22, 20, 28));

    // ── Top chrome bar ────────────────────────────────────────────────────────
    const int BAR = cfg::kBarH;
    {
        HFONT logo  = MakeFont(18, FW_BLACK);
        HFONT brand = MakeFont(15, FW_SEMIBOLD);

        // "V" purple pill
        GdiRect(dc, {24, 12, 56, 40}, rgb(101, 67, 220), rgb(90, 55, 200), 9);
        GdiText(dc, L"V", {24, 12, 56, 40}, logo, rgb(255, 255, 255));

        // "void" label
        GdiText(dc, L"void", {60, 12, 130, 40}, brand, rgb(231, 228, 236),
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // Window controls (right side) — circles
        GdiCircle(dc, W - 30, 26, 7, rgb(42, 36, 64));   // minimize
        GdiCircle(dc, W - 12, 26, 7, rgb(42, 36, 64));   // close

        // Thin separator under bar
        GdiLine(dc, 0, BAR, W, BAR, rgb(28, 24, 38));

        DeleteObject(logo);
        DeleteObject(brand);
    }

    // ── Vertical centre divider (gradient-ish via alpha — fake with steps) ────
    for (int y = BAR + 12; y < H - 24; ++y) {
        float t  = (float)(y - BAR - 12) / (float)(H - 24 - BAR - 12);
        float a  = (t < 0.5f) ? (t * 2.f) : (2.f - t * 2.f);   // bell 0→1→0
        int   v  = (int)(a * 22.f);
        SetPixel(dc, cx, y, rgb(v + 6, v + 4, v + 14));
    }

    // ── Shake offset (applied to left panel only) ─────────────────────────────
    int sx = (int)ShakeX();

    // ═══════════════════════════════════════════════════════════════
    //  LEFT PANEL — PIN input
    // ═══════════════════════════════════════════════════════════════
    if (!st::authed) {
        HFONT fTitle  = MakeFont(30, FW_BOLD);
        HFONT fSub    = MakeFont(13, FW_NORMAL);
        HFONT fBtn    = MakeFont(15, FW_SEMIBOLD);
        HFONT fSmall  = MakeFont(12, FW_NORMAL);

        const int lx  = 38 + sx;   // left margin + shake
        const int ry  = BAR + 32;  // content start Y

        // Title
        GdiText(dc, L"Enter PIN",
                {lx, ry, cx - 10 + sx, ry + 40},
                fTitle, rgb(242, 239, 248), DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // Subtitle
        GdiText(dc, L"Access your void account",
                {lx, ry + 44, cx - 10 + sx, ry + 66},
                fSub, rgb(94, 86, 112), DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // ── 6 PIN boxes ───────────────────────────────────────────────────────
        const int boxW = 56, boxH = 68, gap = 10;
        const int totalPinW = cfg::kPIN * boxW + (cfg::kPIN - 1) * gap;
        const int pinX0 = lx;
        const int pinY0 = ry + 78;
        const int pinCount = (int)st::pin.size();

        for (int i = 0; i < cfg::kPIN; ++i) {
            bool filled  = (i < pinCount);
            bool focused = (i == pinCount && !st::authed);  // next empty box

            RECT box = {
                pinX0 + i * (boxW + gap),
                pinY0,
                pinX0 + i * (boxW + gap) + boxW,
                pinY0 + boxH
            };

            // Box background + border
            COLORREF bgCol  = focused ? rgb(19, 15, 30) : rgb(12, 10, 18);
            COLORREF borCol = focused ? rgb(90, 63, 204) : rgb(30, 26, 40);
            GdiRect(dc, box, bgCol, borCol, 16);

            // Left accent bar when focused
            if (focused) {
                RECT accent = {box.left + 1, box.top + 12, box.left + 4, box.bottom - 12};
                GdiRect(dc, accent, rgb(125, 92, 255), rgb(125, 92, 255), 2);
            }

            // Bullet dot when filled
            if (filled) {
                int cx2 = (box.left + box.right) / 2;
                int cy2 = (box.top + box.bottom) / 2;
                GdiCircle(dc, cx2, cy2, 6, rgb(155, 132, 196));
            }
        }

        // ── Next button + fingerprint icon row ────────────────────────────────
        const int rowY = pinY0 + boxH + 14;
        const int fpSz = 68;
        const int fpX  = pinX0 + totalPinW - fpSz;
        const int btnX = pinX0;
        const int btnW = totalPinW - fpSz - 10;

        // Next button
        {
            RECT btn = {btnX, rowY, btnX + btnW, rowY + fpSz};
            GdiRect(dc, btn, rgb(16, 14, 24), rgb(42, 36, 56), 14);
            GdiText(dc, L"Next  \x2192", btn, fBtn, rgb(222, 218, 240));
        }

        // Fingerprint icon box
        {
            RECT fp = {fpX, rowY, fpX + fpSz, rowY + fpSz};
            GdiRect(dc, fp, rgb(12, 10, 18), rgb(30, 26, 40), 18);

            // Draw fingerprint rings (arcs)
            HPEN fpPen = CreatePen(PS_SOLID, 2, rgb(155, 132, 196));
            HBRUSH nullBr = (HBRUSH)GetStockObject(NULL_BRUSH);
            HGDIOBJ op = SelectObject(dc, fpPen);
            HGDIOBJ ob = SelectObject(dc, nullBr);
            int fcx = fpX + fpSz / 2, fcy = rowY + fpSz / 2;
            for (int r = 8; r <= 22; r += 7)
                Ellipse(dc, fcx - r, fcy - r, fcx + r, fcy + r);
            SelectObject(dc, op);
            SelectObject(dc, ob);
            DeleteObject(fpPen);
        }

        // ── Status / error text ───────────────────────────────────────────────
        if (st::error) {
            GdiText(dc, L"Incorrect PIN",
                    {lx, rowY + fpSz + 8, lx + totalPinW, rowY + fpSz + 30},
                    fSub, rgb(239, 68, 68), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        // ── Footer ────────────────────────────────────────────────────────────
        {
            RECT fr = {lx, H - 40, lx + totalPinW, H - 14};
            GdiText(dc, L"Subscription ending?  Extend it", fr,
                    fSmall, rgb(62, 56, 80), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        DeleteObject(fTitle);
        DeleteObject(fSub);
        DeleteObject(fBtn);
        DeleteObject(fSmall);
    }

    // ═══════════════════════════════════════════════════════════════
    //  RIGHT PANEL — Loader (shown after auth)
    // ═══════════════════════════════════════════════════════════════
    {
        const int rx  = cx + (W - cx) / 2;   // right panel centre X
        const int mid = BAR + (H - BAR) / 2; // vertical centre

        if (!st::authed) {
            // Pre-auth hint
            HFONT f = MakeFont(13, FW_NORMAL);
            GdiText(dc, L"Enter your PIN to continue",
                    {cx + 10, mid - 10, W - 10, mid + 20},
                    f, rgb(55, 50, 68));
            DeleteObject(f);
        } else {
            // Big "V" logo
            HFONT fLogo  = MakeFont(48, FW_BLACK);
            HFONT fBrand = MakeFont(26, FW_BOLD);
            HFONT fPhase = MakeFont(13, FW_NORMAL);

            // Rounded square background
            GdiRect(dc, {rx - 46, mid - 120, rx + 46, mid - 28},
                    rgb(101, 67, 220), rgb(80, 50, 180), 24);
            GdiText(dc, L"V", {rx - 46, mid - 120, rx + 46, mid - 28},
                    fLogo, rgb(255, 255, 255));

            GdiText(dc, L"void", {cx + 10, mid - 20, W - 10, mid + 10},
                    fBrand, rgb(231, 228, 236));

            // Phase text
            static const wchar_t* phases[] = {
                L"Authenticating\x2026", L"Loading modules\x2026",
                L"Connecting\x2026",     L"Finalizing\x2026", L"Ready."
            };
            int phase = st::progress / 25;
            if (phase >= 5) phase = 4;
            GdiText(dc, phases[phase],
                    {cx + 10, mid + 14, W - 10, mid + 38},
                    fPhase, rgb(94, 86, 112));

            // Progress bar track
            const int pbX = cx + 30, pbW = W - cx - 60;
            const int pbY = mid + 50, pbH = 6;
            GdiRect(dc, {pbX, pbY, pbX + pbW, pbY + pbH},
                    rgb(26, 22, 37), rgb(26, 22, 37), 3);

            // Progress bar fill
            int fillW = pbW * st::progress / 100;
            if (fillW > 0) {
                GdiRect(dc, {pbX, pbY, pbX + fillW, pbY + pbH},
                        rgb(139, 92, 246), rgb(139, 92, 246), 3);
            }

            DeleteObject(fLogo);
            DeleteObject(fBrand);
            DeleteObject(fPhase);
        }
    }
}

// ── Hit tests ─────────────────────────────────────────────────────────────────
static bool HitClose(int x, int y, int W) {
    return abs(x - (W - 12)) <= 8 && abs(y - 26) <= 8;
}
static bool HitMinimize(int x, int y, int W) {
    return abs(x - (W - 30)) <= 8 && abs(y - 26) <= 8;
}
static bool HitNextBtn(int x, int y) {
    if (st::authed) return false;
    const int lx   = 38, pinX0 = lx;
    const int boxW = 56, gap = 10;
    const int fpSz = 68;
    const int totalPinW = cfg::kPIN * boxW + (cfg::kPIN - 1) * gap;
    const int btnW = totalPinW - fpSz - 10;
    const int rowY = cfg::kBarH + 32 + 78 + 68 + 14;
    return x >= pinX0 && x <= pinX0 + btnW && y >= rowY && y <= rowY + fpSz;
}
static bool HitTopBar(int y) { return y < cfg::kBarH; }

// ── Submission logic ──────────────────────────────────────────────────────────
static void TrySubmit(HWND hwnd) {
    if ((int)st::pin.size() < cfg::kPIN) return;
    if (st::pin == cfg::kCorrectPin) {
        st::error  = false;
        st::authed = true;
        SetTimer(hwnd, st::timerId, 30, nullptr);
    } else {
        st::error    = true;
        st::shaking  = true;
        st::shakeStart = GetTickCount();
        st::pin.clear();
        // Redraw repeatedly while shaking
        SetTimer(hwnd, st::timerId + 1, 16, nullptr);
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

// ── WndProc ───────────────────────────────────────────────────────────────────
static HWND g_hwnd = nullptr;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    const int mx = LOWORD(lp), my = HIWORD(lp);
    RECT wr{}; GetWindowRect(hwnd, &wr);
    const int W = wr.right - wr.left;

    switch (msg) {
    // ── Setup ─────────────────────────────────────────────────────────────────
    case WM_CREATE: {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        DWORD policy = DWMNCRP_ENABLED;
        DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
        return 0;
    }

    // ── Paint (double-buffered) ───────────────────────────────────────────────
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT cl{}; GetClientRect(hwnd, &cl);
        // Off-screen buffer
        HDC mdc = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, cl.right, cl.bottom);
        HGDIOBJ old = SelectObject(mdc, bmp);
        DrawUI(mdc, hwnd);
        BitBlt(dc, 0, 0, cl.right, cl.bottom, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, old);
        DeleteObject(bmp);
        DeleteDC(mdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;

    // ── Timer ─────────────────────────────────────────────────────────────────
    case WM_TIMER:
        if (wp == st::timerId && st::authed) {
            // Progress animation (~3.5 s)
            st::progress = (st::progress + 2 < 100) ? st::progress + 2 : 100;
            InvalidateRect(hwnd, nullptr, FALSE);
            if (st::progress >= 100) {
                KillTimer(hwnd, st::timerId);
                LaunchExe(hwnd);
            }
        }
        if (wp == st::timerId + 1) {
            // Shake redraw timer
            if (!st::shaking) KillTimer(hwnd, st::timerId + 1);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    // ── Keyboard ──────────────────────────────────────────────────────────────
    case WM_CHAR: {
        if (st::authed) break;
        wchar_t ch = (wchar_t)wp;
        if (ch >= L'0' && ch <= L'9' && (int)st::pin.size() < cfg::kPIN) {
            st::error = false;
            st::pin.push_back(ch);
            InvalidateRect(hwnd, nullptr, FALSE);
            if ((int)st::pin.size() == cfg::kPIN) TrySubmit(hwnd);
        } else if (ch == L'\b' && !st::pin.empty()) {
            st::error = false;
            st::pin.pop_back();
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (ch == L'\r') {
            TrySubmit(hwnd);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return 0;

    // ── Mouse ─────────────────────────────────────────────────────────────────
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        if (HitClose(mx, my, W)) { PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0; }
        if (HitMinimize(mx, my, W)) { ShowWindow(hwnd, SW_MINIMIZE); return 0; }
        if (HitNextBtn(mx, my)) { TrySubmit(hwnd); return 0; }
        if (HitTopBar(my)) {
            st::dragging = true;
            GetCursorPos(&st::dragOrig);
            st::wndOrig = {wr.left, wr.top};
            SetCapture(hwnd);
        }
        return 0;

    case WM_MOUSEMOVE:
        if (st::dragging) {
            POINT cur{}; GetCursorPos(&cur);
            SetWindowPos(hwnd, nullptr,
                         st::wndOrig.x + cur.x - st::dragOrig.x,
                         st::wndOrig.y + cur.y - st::dragOrig.y,
                         0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }
        return 0;

    case WM_LBUTTONUP:
        if (st::dragging) { st::dragging = false; ReleaseCapture(); }
        return 0;

    // ── Teardown ──────────────────────────────────────────────────────────────
    case WM_DESTROY:
        KillTimer(hwnd, st::timerId);
        KillTimer(hwnd, st::timerId + 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── Entry point ───────────────────────────────────────────────────────────────
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nShow) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance     = hInst;
    wc.lpfnWndProc   = WndProc;
    wc.lpszClassName = cfg::kClass;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    RegisterClassExW(&wc);

    const DWORD style   = WS_POPUP | WS_VISIBLE;
    const DWORD exStyle = WS_EX_APPWINDOW;

    g_hwnd = CreateWindowExW(exStyle, cfg::kClass, L"void",
                             style, 0, 0, cfg::kW, cfg::kH,
                             nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;

    // Centre on screen
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(g_hwnd, nullptr,
                 (sw - cfg::kW) / 2, (sh - cfg::kH) / 2,
                 cfg::kW, cfg::kH, SWP_NOZORDER | SWP_SHOWWINDOW);

    SetFocus(g_hwnd);
    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
