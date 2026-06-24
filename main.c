/*
 * Look20 - a 20-20-20 eye-break reminder for Windows.
 *
 * Every <work> minutes it slides an animated pixel-art "eye" overlay onto your
 * screen(s) reminding you to look ~20 feet away for 20 seconds, with a live
 * countdown and Skip / Snooze controls. Lives in the system tray; everything
 * is drawn procedurally (no image assets) so the whole thing is one small .exe.
 *
 * The overlay can mirror onto every monitor at once. The 20-20-20 rule: every
 * 20 minutes, look at something ~20 feet (6 m) away for at least 20 seconds to
 * relax the eyes' focusing muscles.
 *
 * Pure Win32 C. Build with MinGW (see build.bat / README.md).
 */

#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include "resource.h"

/* ------------------------------------------------------------------ */
/* Constants                                                          */
/* ------------------------------------------------------------------ */

#define APP_NAME            L"Look20"
#define APP_CLASS_MAIN      L"Look20_MainWnd"
#define APP_CLASS_OVERLAY   L"Look20_OverlayWnd"
#define APP_MUTEX           L"Look20_SingleInstance_Mutex_{6af327c6}"
#define REG_PATH            L"Software\\Look20"
#define RUN_PATH            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE           L"Look20"

/* Timers (both live on the main window) */
#define TIMER_WORK          1       /* 1 second work-interval tick          */
#define TIMER_ANIM          2       /* ~60 fps overlay animation tick       */
#define WORK_TICK_MS        1000
#define ANIM_TICK_MS        16
#define SLIDE_MS            420.0   /* slide in / out duration              */
#define IDLE_THRESHOLD_MS   60000   /* "user is away" cut-off               */
#define FULLSCREEN_RETRY_S  60      /* postpone if a fullscreen app is up   */

/* Low-resolution art canvas. Everything is drawn here, then scaled up with
 * nearest-neighbour sampling to produce the chunky pixel-art look. */
#define LOW                 160
#define LOH                 58

/* Logical (96-dpi) overlay size; scaled by each monitor's DPI.
 * Kept proportional to LOW:LOH so the upscaled "pixels" stay square. */
#define BASE_W              468
#define BASE_H              170
#define EDGE_MARGIN         24      /* gap from the screen work-area edge   */

#define MAX_OVERLAYS        16

/* Per-pixel transparency via a colour key: any pixel painted with this exact
 * colour becomes fully transparent (and click-through). */
#define COLORKEY            RGB(255, 0, 255)
#define WIN_ALPHA           240     /* slight overall translucency (0..255) */

/* Theme */
#define COL_BUBBLE          RGB(30, 35, 52)
#define COL_BUBBLE_EDGE     RGB(74, 102, 170)
#define COL_ACCENT          RGB(96, 156, 255)
#define COL_ACCENT_DIM      RGB(54, 84, 140)
#define COL_TEXT            RGB(236, 240, 248)
#define COL_TEXT_DIM        RGB(150, 162, 188)
#define COL_SCLERA          RGB(232, 238, 248)
#define COL_IRIS            RGB(150, 100, 44)   /* dark honey-brown eye */
#define COL_IRIS_RIM        RGB(96, 62, 26)     /* darker brown outer rim */
#define COL_PUPIL           RGB(18, 22, 34)
#define COL_BTN             RGB(44, 52, 76)
#define COL_BTN_HOT         RGB(60, 84, 140)
#define COL_TRACK           RGB(48, 55, 78)

/* Overlay phases */
enum { PH_SLIDE_IN, PH_HOLD, PH_SLIDE_OUT };

/* Slide-in directions (which edge the overlay emerges from) */
enum { SLIDE_UP, SLIDE_DOWN, SLIDE_LEFT, SLIDE_RIGHT };

/* Overlay positions (relative to each monitor's work area) */
enum {
    POS_TOP_RIGHT, POS_BOTTOM_RIGHT, POS_RIGHT_CENTER,
    POS_TOP_LEFT, POS_BOTTOM_LEFT, POS_BOTTOM_CENTER,
    POS_FACING, POS_CUSTOM, POS_COUNT
};

/* Result of a break (why the overlay closed) */
enum { END_DONE, END_SKIP, END_SNOOZE };

/* ------------------------------------------------------------------ */
/* Settings                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    int  workMinutes;
    int  breakSeconds;
    int  snoozeMinutes;
    int  position;
    int  customX, customY;   /* offset from a monitor's work-area top-left */
    BOOL sound;
    BOOL autostart;
    BOOL pauseWhenIdle;
    BOOL skipFullscreen;
    BOOL allMonitors;
} Settings;

static const int WORK_CHOICES[]   = { 15, 20, 25, 30, 45, 60 };
static const int BREAK_CHOICES[]  = { 20, 30, 60 };
static const int SNOOZE_CHOICES[] = { 3, 5, 10 };
#define NCHOICES(a) ((int)(sizeof(a)/sizeof((a)[0])))

/* ------------------------------------------------------------------ */
/* One overlay window per monitor                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    HWND   hwnd;
    int    winW, winH;
    double scaleX, scaleY;     /* window / low-res scale factors */
    int    slideDir;           /* SLIDE_* edge to emerge from     */
    RECT   target;             /* on-screen resting rectangle    */
    RECT   offscreen;          /* slide start/end rectangle       */
    RECT   slideOrigin;        /* where slide-out starts from     */
    RECT   homeMon;            /* this overlay's monitor (for clip)*/
    RECT   rcSkip, rcSnooze;   /* button hit rects (client coords)*/
    int    hot;                /* 0 none, 1 skip, 2 snooze        */
    BOOL   dragging;
    POINT  dragGrab;
} Overlay;

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */

static HINSTANCE g_hInst;
static HWND      g_hMain;
static NOTIFYICONDATAW g_nid;
static HICON     g_trayIcon;
static Settings  g_cfg;

static BOOL      g_paused      = FALSE;
static int       g_secsLeft    = 0;        /* seconds until next break      */
static BOOL      g_breakActive = FALSE;

static Overlay   g_ov[MAX_OVERLAYS];
static int       g_ovCount     = 0;

/* monitors targeted for the current break */
static HMONITOR  g_monHandle[MAX_OVERLAYS];
static RECT      g_monRect[MAX_OVERLAYS];
static int       g_monCount    = 0;

/* Shared break state (all overlays animate in lock-step) */
static int        g_phase       = PH_SLIDE_IN;
static ULONGLONG  g_phaseStart  = 0;
static ULONGLONG  g_breakEnd    = 0;
static ULONGLONG  g_prevTick    = 0;
static int        g_breakResult = END_DONE;

/* Shared low-res render target + fonts (single UI thread) */
static HDC        g_loDC  = NULL;
static HBITMAP    g_loBmp = NULL;
static HBITMAP    g_loOld = NULL;
static HFONT      g_fHead = NULL, g_fBody = NULL, g_fBtn = NULL, g_fNum = NULL;

/* Dynamically resolved DPI helper (shcore) */
typedef HRESULT (WINAPI *PFN_GetDpiForMonitor)(HMONITOR, int, UINT*, UINT*);
static PFN_GetDpiForMonitor p_GetDpiForMonitor = NULL;

/* ------------------------------------------------------------------ */
/* Forward declarations                                                */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK MainWndProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK OverlayWndProc(HWND, UINT, WPARAM, LPARAM);
static void StartBreak(void);
static void BeginSlideOut(int result);
static void FinishBreak(void);
static void ResetCountdown(void);

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* cubic ease-out, t in [0,1] */
static double easeOut(double t) {
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    double u = 1.0 - t;
    return 1.0 - u * u * u;
}

static UINT MonitorDpi(HMONITOR hMon) {
    if (p_GetDpiForMonitor) {
        UINT dx = 96, dy = 96;
        if (SUCCEEDED(p_GetDpiForMonitor(hMon, 0 /*MDT_EFFECTIVE_DPI*/, &dx, &dy)))
            return dx ? dx : 96;
    }
    return 96;
}

static RECT MonitorWorkArea(HMONITOR mon) {
    MONITORINFO mi; mi.cbSize = sizeof(mi);
    RECT wa;
    if (GetMonitorInfoW(mon, &mi))
        wa = mi.rcWork;
    else if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0))
        SetRect(&wa, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    return wa;
}

static Overlay *OvFromHwnd(HWND h) {
    for (int i = 0; i < g_ovCount; ++i)
        if (g_ov[i].hwnd == h) return &g_ov[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Settings persistence                                                */
/* ------------------------------------------------------------------ */

static DWORD RegGetDword(HKEY h, const wchar_t *name, DWORD def) {
    DWORD val = def, sz = sizeof(val), type = 0;
    if (RegQueryValueExW(h, name, NULL, &type, (BYTE*)&val, &sz) != ERROR_SUCCESS
        || type != REG_DWORD)
        return def;
    return val;
}

static void LoadConfig(void) {
    g_cfg.workMinutes    = 20;
    g_cfg.breakSeconds   = 20;
    g_cfg.snoozeMinutes  = 5;
    g_cfg.position       = POS_FACING;
    g_cfg.customX        = 0;
    g_cfg.customY        = 0;
    g_cfg.sound          = TRUE;
    g_cfg.autostart      = FALSE;
    g_cfg.pauseWhenIdle  = TRUE;
    g_cfg.skipFullscreen = TRUE;
    g_cfg.allMonitors    = TRUE;

    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, KEY_READ, &h) == ERROR_SUCCESS) {
        g_cfg.workMinutes    = (int)RegGetDword(h, L"WorkMinutes",   g_cfg.workMinutes);
        g_cfg.breakSeconds   = (int)RegGetDword(h, L"BreakSeconds",  g_cfg.breakSeconds);
        g_cfg.snoozeMinutes  = (int)RegGetDword(h, L"SnoozeMinutes", g_cfg.snoozeMinutes);
        g_cfg.position       = (int)RegGetDword(h, L"Position",      g_cfg.position);
        g_cfg.customX        = (int)RegGetDword(h, L"CustomX",       g_cfg.customX);
        g_cfg.customY        = (int)RegGetDword(h, L"CustomY",       g_cfg.customY);
        g_cfg.sound          = (BOOL)RegGetDword(h, L"Sound",          g_cfg.sound);
        g_cfg.autostart      = (BOOL)RegGetDword(h, L"Autostart",      g_cfg.autostart);
        g_cfg.pauseWhenIdle  = (BOOL)RegGetDword(h, L"PauseWhenIdle",  g_cfg.pauseWhenIdle);
        g_cfg.skipFullscreen = (BOOL)RegGetDword(h, L"SkipFullscreen", g_cfg.skipFullscreen);
        g_cfg.allMonitors    = (BOOL)RegGetDword(h, L"AllMonitors",    g_cfg.allMonitors);
        RegCloseKey(h);
    }

    g_cfg.workMinutes   = clampi(g_cfg.workMinutes,   1, 240);
    g_cfg.breakSeconds  = clampi(g_cfg.breakSeconds,  5, 600);
    g_cfg.snoozeMinutes = clampi(g_cfg.snoozeMinutes, 1, 120);
    g_cfg.position      = clampi(g_cfg.position, 0, POS_COUNT - 1);
}

static void SaveConfig(void) {
    HKEY h;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, NULL, 0,
                        KEY_WRITE, NULL, &h, NULL) != ERROR_SUCCESS)
        return;
    DWORD v;
    #define PUT(name, value) do { v = (DWORD)(value); \
        RegSetValueExW(h, name, 0, REG_DWORD, (BYTE*)&v, sizeof(v)); } while (0)
    PUT(L"WorkMinutes",    g_cfg.workMinutes);
    PUT(L"BreakSeconds",   g_cfg.breakSeconds);
    PUT(L"SnoozeMinutes",  g_cfg.snoozeMinutes);
    PUT(L"Position",       g_cfg.position);
    PUT(L"CustomX",        g_cfg.customX);
    PUT(L"CustomY",        g_cfg.customY);
    PUT(L"Sound",          g_cfg.sound);
    PUT(L"Autostart",      g_cfg.autostart);
    PUT(L"PauseWhenIdle",  g_cfg.pauseWhenIdle);
    PUT(L"SkipFullscreen", g_cfg.skipFullscreen);
    PUT(L"AllMonitors",    g_cfg.allMonitors);
    #undef PUT
    RegCloseKey(h);
}

static void ApplyAutostart(BOOL enable) {
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_PATH, 0, KEY_SET_VALUE, &h) != ERROR_SUCCESS)
        return;
    if (enable) {
        wchar_t path[MAX_PATH + 2];
        path[0] = L'"';
        DWORD n = GetModuleFileNameW(NULL, path + 1, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            path[n + 1] = L'"';
            path[n + 2] = 0;
            RegSetValueExW(h, RUN_VALUE, 0, REG_SZ,
                           (BYTE*)path, (DWORD)((wcslen(path) + 1) * sizeof(wchar_t)));
        }
    } else {
        RegDeleteValueW(h, RUN_VALUE);
    }
    RegCloseKey(h);
}

/* ------------------------------------------------------------------ */
/* Gentle chime                                                        */
/* ------------------------------------------------------------------ */

static void PlayChime(BOOL rising) {
    (void)rising;
    if (!g_cfg.sound) return;
    /* Soft, volume-respecting notification "ding". SND_ASYNC returns at once;
     * SND_NODEFAULT keeps it silent (no harsh fallback) if the alias is
     * missing. This replaces Beep(), which bypasses the volume mixer and is
     * jarringly loud. */
    PlaySoundW(L"Notification.Default", NULL, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
}

/* ------------------------------------------------------------------ */
/* Idle / fullscreen detection                                         */
/* ------------------------------------------------------------------ */

static DWORD IdleMillis(void) {
    LASTINPUTINFO lii;
    lii.cbSize = sizeof(lii);
    if (!GetLastInputInfo(&lii)) return 0;
    return GetTickCount() - lii.dwTime;   /* unsigned wrap is fine */
}

static BOOL ForegroundIsFullscreen(void) {
    HWND fg = GetForegroundWindow();
    if (!fg) return FALSE;
    if (fg == GetDesktopWindow() || fg == GetShellWindow()) return FALSE;
    if (fg == g_hMain) return FALSE;

    RECT wr;
    if (!GetWindowRect(fg, &wr)) return FALSE;

    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return FALSE;

    return (wr.left   <= mi.rcMonitor.left   + 1 &&
            wr.top    <= mi.rcMonitor.top    + 1 &&
            wr.right  >= mi.rcMonitor.right  - 1 &&
            wr.bottom >= mi.rcMonitor.bottom - 1);
}

/* ------------------------------------------------------------------ */
/* Tray icon                                                           */
/* ------------------------------------------------------------------ */

static HICON CreateEyeIcon(int size) {
    HDC screen = GetDC(NULL);
    HDC dc = CreateCompatibleDC(screen);

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = size;
    bi.bmiHeader.biHeight      = -size;          /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HBITMAP oldb  = (HBITMAP)SelectObject(dc, color);

    if (bits) memset(bits, 0, (size_t)size * size * 4);

    SetBkMode(dc, TRANSPARENT);
    double f = size;

    HBRUSH bgBrush = CreateSolidBrush(RGB(28, 33, 48));
    HPEN   noPen   = (HPEN)GetStockObject(NULL_PEN);
    HGDIOBJ oldBr  = SelectObject(dc, bgBrush);
    HGDIOBJ oldPn  = SelectObject(dc, noPen);
    int r = (int)(f * 0.30);
    RoundRect(dc, 0, 0, size, size, r, r);

    HBRUSH scl = CreateSolidBrush(COL_SCLERA);
    SelectObject(dc, scl);
    Ellipse(dc, (int)(f*0.10), (int)(f*0.28), (int)(f*0.90), (int)(f*0.72));

    HBRUSH iris = CreateSolidBrush(COL_IRIS);
    SelectObject(dc, iris);
    int ir = (int)(f * 0.34);
    Ellipse(dc, (size-ir)/2, (size-ir)/2, (size+ir)/2, (size+ir)/2);

    HBRUSH pup = CreateSolidBrush(COL_PUPIL);
    SelectObject(dc, pup);
    int pr = (int)(f * 0.16);
    Ellipse(dc, (size-pr)/2, (size-pr)/2, (size+pr)/2, (size+pr)/2);

    HBRUSH hl = CreateSolidBrush(RGB(255, 255, 255));
    SelectObject(dc, hl);
    int hr = (int)(f * 0.09); if (hr < 1) hr = 1;
    int hx = (int)(f * 0.42), hy = (int)(f * 0.40);
    Ellipse(dc, hx, hy, hx + hr, hy + hr);

    /* GDI leaves the alpha byte at 0; force opaque where we drew. */
    if (bits) {
        unsigned char *p = (unsigned char*)bits;
        for (int i = 0; i < size * size; ++i) {
            if (p[0] || p[1] || p[2]) p[3] = 255;
            p += 4;
        }
    }

    SelectObject(dc, oldBr);
    SelectObject(dc, oldPn);
    SelectObject(dc, oldb);
    DeleteObject(bgBrush); DeleteObject(scl); DeleteObject(iris);
    DeleteObject(pup);     DeleteObject(hl);

    HBITMAP mask = CreateBitmap(size, size, 1, 1, NULL);

    ICONINFO ii;
    ii.fIcon    = TRUE;
    ii.xHotspot = 0;
    ii.yHotspot = 0;
    ii.hbmMask  = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(mask);
    DeleteObject(color);
    DeleteDC(dc);
    ReleaseDC(NULL, screen);
    return icon;
}

static void TrayTip(const wchar_t *text) {
    wcsncpy(g_nid.szTip, text, ARRAYSIZE(g_nid.szTip) - 1);
    g_nid.szTip[ARRAYSIZE(g_nid.szTip) - 1] = 0;
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void UpdateTrayTip(void) {
    wchar_t buf[128];
    if (g_breakActive)
        wcscpy(buf, L"Look20 - break in progress");
    else if (g_paused)
        wcscpy(buf, L"Look20 - paused");
    else {
        int m = g_secsLeft / 60, s = g_secsLeft % 60;
        swprintf(buf, ARRAYSIZE(buf), L"Look20 - next break in %d:%02d", m, s);
    }
    TrayTip(buf);
}

static void AddTray(void) {
    int sm = GetSystemMetrics(SM_CXSMICON);
    if (sm <= 0) sm = 16;
    if (g_trayIcon) { DestroyIcon(g_trayIcon); g_trayIcon = NULL; }  /* idempotent re-add */
    g_trayIcon = CreateEyeIcon(sm);

    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = g_hMain;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = g_trayIcon;
    wcscpy(g_nid.szTip, L"Look20");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void RemoveTray(void) {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_trayIcon) { DestroyIcon(g_trayIcon); g_trayIcon = NULL; }
}

/* ------------------------------------------------------------------ */
/* Context menu                                                         */
/* ------------------------------------------------------------------ */

static void AppendRadio(HMENU m, int base, const int *choices, int n, int current,
                        const wchar_t *suffix) {
    for (int i = 0; i < n; ++i) {
        wchar_t lbl[64];
        swprintf(lbl, ARRAYSIZE(lbl), L"%d %ls", choices[i], suffix);
        AppendMenuW(m, MF_STRING, base + choices[i], lbl);
        if (choices[i] == current)
            CheckMenuRadioItem(m, base + choices[0], base + choices[n-1],
                               base + choices[i], MF_BYCOMMAND);
    }
}

static void ShowMenu(void) {
    HMENU pos = CreatePopupMenu();
    static const wchar_t *POS_NAMES[POS_COUNT] = {
        L"Top right", L"Bottom right", L"Right center",
        L"Top left", L"Bottom left", L"Bottom center",
        L"Face the shared edge (multi-monitor)",
        L"Custom (drag overlay to set)"
    };
    for (int i = 0; i < POS_COUNT; ++i) {
        AppendMenuW(pos, MF_STRING, IDM_POS_BASE + i, POS_NAMES[i]);
        if (i == g_cfg.position)
            CheckMenuRadioItem(pos, IDM_POS_BASE, IDM_POS_BASE + POS_COUNT - 1,
                               IDM_POS_BASE + i, MF_BYCOMMAND);
    }

    HMENU work = CreatePopupMenu();
    AppendRadio(work, IDM_WORK_BASE, WORK_CHOICES, NCHOICES(WORK_CHOICES),
                g_cfg.workMinutes, L"min");

    HMENU brk = CreatePopupMenu();
    AppendRadio(brk, IDM_BREAK_BASE, BREAK_CHOICES, NCHOICES(BREAK_CHOICES),
                g_cfg.breakSeconds, L"sec");

    HMENU snz = CreatePopupMenu();
    AppendRadio(snz, IDM_SNOOZE_BASE, SNOOZE_CHOICES, NCHOICES(SNOOZE_CHOICES),
                g_cfg.snoozeMinutes, L"min");

    HMENU settings = CreatePopupMenu();
    AppendMenuW(settings, MF_POPUP, (UINT_PTR)work, L"Work interval");
    AppendMenuW(settings, MF_POPUP, (UINT_PTR)brk,  L"Break length");
    AppendMenuW(settings, MF_POPUP, (UINT_PTR)snz,  L"Snooze length");
    AppendMenuW(settings, MF_POPUP, (UINT_PTR)pos,  L"Overlay position");
    AppendMenuW(settings, MF_SEPARATOR, 0, NULL);
    AppendMenuW(settings, MF_STRING | (g_cfg.allMonitors    ? MF_CHECKED : 0),
                IDM_ALL_MONITORS,   L"Show on all monitors");
    AppendMenuW(settings, MF_STRING | (g_cfg.sound          ? MF_CHECKED : 0),
                IDM_SOUND,          L"Sound");
    AppendMenuW(settings, MF_STRING | (g_cfg.pauseWhenIdle  ? MF_CHECKED : 0),
                IDM_PAUSE_IDLE,     L"Pause when I'm away");
    AppendMenuW(settings, MF_STRING | (g_cfg.skipFullscreen ? MF_CHECKED : 0),
                IDM_SKIP_FULLSCREEN,L"Don't interrupt fullscreen apps");
    AppendMenuW(settings, MF_STRING | (g_cfg.autostart      ? MF_CHECKED : 0),
                IDM_AUTOSTART,      L"Start with Windows");

    HMENU m = CreatePopupMenu();
    wchar_t head[128];
    if (g_breakActive)   wcscpy(head, L"Break in progress");
    else if (g_paused)   wcscpy(head, L"Paused");
    else {
        int mm = g_secsLeft / 60, ss = g_secsLeft % 60;
        swprintf(head, ARRAYSIZE(head), L"Next break in %d:%02d", mm, ss);
    }
    AppendMenuW(m, MF_STRING | MF_GRAYED | MF_DISABLED, IDM_HEADER, head);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_BREAK_NOW, L"Take a break now");
    AppendMenuW(m, MF_STRING | (g_paused ? MF_CHECKED : 0), IDM_PAUSE,
                g_paused ? L"Resume" : L"Pause");
    AppendMenuW(m, MF_STRING, IDM_RESET, L"Reset timer");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)settings, L"Settings");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_ABOUT, L"About Look20");
    AppendMenuW(m, MF_STRING, IDM_EXIT,  L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hMain);   /* so the menu dismisses on outside click */
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hMain, NULL);
    PostMessageW(g_hMain, WM_NULL, 0, 0);
    DestroyMenu(m);   /* destroys submenus too */
}

/* ------------------------------------------------------------------ */
/* Countdown / balloon                                                 */
/* ------------------------------------------------------------------ */

static void ResetCountdown(void) {
    g_secsLeft = g_cfg.workMinutes * 60;
}

static void ShowBalloon(const wchar_t *title, const wchar_t *text) {
    g_nid.uFlags = NIF_INFO;
    wcsncpy(g_nid.szInfoTitle, title, ARRAYSIZE(g_nid.szInfoTitle) - 1);
    g_nid.szInfoTitle[ARRAYSIZE(g_nid.szInfoTitle) - 1] = 0;
    wcsncpy(g_nid.szInfo, text, ARRAYSIZE(g_nid.szInfo) - 1);
    g_nid.szInfo[ARRAYSIZE(g_nid.szInfo) - 1] = 0;
    g_nid.dwInfoFlags = NIIF_NONE;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_TIP;
}

/* ------------------------------------------------------------------ */
/* Overlay geometry                                                    */
/* ------------------------------------------------------------------ */

static BOOL CALLBACK CollectMon(HMONITOR m, HDC dc, LPRECT r, LPARAM l) {
    (void)dc; (void)l;
    if (g_monCount < MAX_OVERLAYS) {
        g_monHandle[g_monCount] = m;
        g_monRect[g_monCount]   = *r;
        g_monCount++;
    }
    return TRUE;
}

/* Is there another monitor adjacent on the given side (dir>0 right, dir<0 left)
 * with some vertical overlap? Used to pick the "inner" (shared) edge. */
static BOOL MonitorHasNeighbor(RECT me, int dir) {
    for (int i = 0; i < g_monCount; ++i) {
        RECT o = g_monRect[i];
        if (EqualRect(&o, &me)) continue;
        BOOL vOverlap = (o.top < me.bottom && o.bottom > me.top);
        if (!vOverlap) continue;
        if (dir > 0 && o.left  >= me.right - 100) return TRUE;
        if (dir < 0 && o.right <= me.left  + 100) return TRUE;
    }
    return FALSE;
}

/* Build ov->offscreen by sliding ov->target off the edge given by ov->slideDir.
 * Vertical slides keep the window on its own monitor; the horizontal (facing)
 * slide deliberately emerges from the shared seam. WM_DPICHANGED is ignored
 * mid-slide so a cross-seam horizontal slide doesn't corrupt the size. */
static void ComputeOffscreen(Overlay *ov, RECT wa) {
    ov->offscreen = ov->target;
    switch (ov->slideDir) {
    case SLIDE_DOWN:  OffsetRect(&ov->offscreen, 0, (wa.top - ov->winH) - ov->target.top); break;
    case SLIDE_LEFT:  OffsetRect(&ov->offscreen, (wa.left - ov->winW) - ov->target.left, 0); break;
    case SLIDE_RIGHT: OffsetRect(&ov->offscreen, wa.right - ov->target.left, 0); break;
    case SLIDE_UP:
    default:          OffsetRect(&ov->offscreen, 0, wa.bottom - ov->target.top); break;
    }
}

static void PlaceOverlayOnMonitor(Overlay *ov, HMONITOR mon, RECT monRect) {
    ov->homeMon = monRect;
    RECT wa = MonitorWorkArea(mon);
    double scale = MonitorDpi(mon) / 96.0;
    ov->winW = (int)(BASE_W * scale);
    ov->winH = (int)(BASE_H * scale);
    ov->scaleX = (double)ov->winW / LOW;
    ov->scaleY = (double)ov->winH / LOH;
    int margin = (int)(EDGE_MARGIN * scale);

    int x, y;
    switch (g_cfg.position) {
    case POS_TOP_RIGHT:
        x = wa.right - ov->winW - margin;  y = wa.top + margin;
        ov->slideDir = SLIDE_DOWN; break;
    case POS_RIGHT_CENTER:
        x = wa.right - ov->winW - margin;  y = (wa.top + wa.bottom - ov->winH) / 2;
        ov->slideDir = SLIDE_UP; break;
    case POS_TOP_LEFT:
        x = wa.left + margin;              y = wa.top + margin;
        ov->slideDir = SLIDE_DOWN; break;
    case POS_BOTTOM_LEFT:
        x = wa.left + margin;              y = wa.bottom - ov->winH - margin;
        ov->slideDir = SLIDE_UP; break;
    case POS_BOTTOM_CENTER:
        x = (wa.left + wa.right - ov->winW) / 2; y = wa.bottom - ov->winH - margin;
        ov->slideDir = SLIDE_UP; break;
    case POS_FACING: {
        /* rest at the edge facing the neighbouring monitor and emerge from that
         * shared edge */
        if (MonitorHasNeighbor(monRect, +1)) {
            x = wa.right - ov->winW - margin;  ov->slideDir = SLIDE_RIGHT;
        } else if (MonitorHasNeighbor(monRect, -1)) {
            x = wa.left + margin;              ov->slideDir = SLIDE_LEFT;
        } else {
            x = wa.right - ov->winW - margin;  ov->slideDir = SLIDE_RIGHT;
        }
        /* Align all facing overlays to the shared vertical centre (the overlap
         * of every monitor's range) so they sit level across the seam even when
         * the monitors differ in height/orientation. */
        int top = monRect.top, bot = monRect.bottom;
        for (int i = 0; i < g_monCount; ++i) {
            if (g_monRect[i].top    > top) top = g_monRect[i].top;
            if (g_monRect[i].bottom < bot) bot = g_monRect[i].bottom;
        }
        if (bot <= top) { top = monRect.top; bot = monRect.bottom; }  /* no overlap */
        y = (top + bot) / 2 - ov->winH / 2;
        if (y < wa.top + margin) y = wa.top + margin;
        if (y + ov->winH > wa.bottom - margin) y = wa.bottom - ov->winH - margin;
        break;
    }
    case POS_CUSTOM:
        /* customX/customY are an offset from this monitor's work-area corner */
        x = wa.left + g_cfg.customX;
        y = wa.top  + g_cfg.customY;
        if (x < wa.left - ov->winW + 40 || x > wa.right - 40 ||
            y < wa.top - 10            || y > wa.bottom - 40) {
            x = wa.right - ov->winW - margin; y = wa.bottom - ov->winH - margin;
        }
        ov->slideDir = SLIDE_UP; break;
    case POS_BOTTOM_RIGHT:
    default:
        x = wa.right - ov->winW - margin;  y = wa.bottom - ov->winH - margin;
        ov->slideDir = SLIDE_UP; break;
    }

    SetRect(&ov->target, x, y, x + ov->winW, y + ov->winH);
    ComputeOffscreen(ov, wa);
}

/* ------------------------------------------------------------------ */
/* Overlay drawing                                                     */
/* ------------------------------------------------------------------ */

static HFONT MakePixelFont(int height, int weight, const wchar_t *face) {
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

static void EnsureOverlayResources(void) {
    if (g_loDC) return;
    HDC screen = GetDC(NULL);
    g_loDC = CreateCompatibleDC(screen);
    g_loBmp = CreateCompatibleBitmap(screen, LOW, LOH);
    g_loOld = (HBITMAP)SelectObject(g_loDC, g_loBmp);
    ReleaseDC(NULL, screen);

    g_fHead = MakePixelFont(16, FW_BOLD,   L"Tahoma");
    g_fBody = MakePixelFont(9,  FW_NORMAL, L"Tahoma");
    g_fBtn  = MakePixelFont(9,  FW_BOLD,   L"Tahoma");
    g_fNum  = MakePixelFont(13, FW_BOLD,   L"Consolas");
}

static void FreeOverlayResources(void) {
    if (g_loDC) {
        SelectObject(g_loDC, g_loOld);
        DeleteObject(g_loBmp);
        DeleteDC(g_loDC);
        g_loDC = NULL; g_loBmp = NULL; g_loOld = NULL;
    }
    if (g_fHead) { DeleteObject(g_fHead); g_fHead = NULL; }
    if (g_fBody) { DeleteObject(g_fBody); g_fBody = NULL; }
    if (g_fBtn)  { DeleteObject(g_fBtn);  g_fBtn  = NULL; }
    if (g_fNum)  { DeleteObject(g_fNum);  g_fNum  = NULL; }
}

static void FillRoundRect(HDC dc, int l, int t, int r, int b, int rad,
                          COLORREF fill, COLORREF edge) {
    HBRUSH br = CreateSolidBrush(fill);
    HPEN   pn = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, pn);
    RoundRect(dc, l, t, r, b, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(br); DeleteObject(pn);
}

static void DrawEye(HDC dc, int cx, int cy, int rx, int ry, double openF,
                    int glanceX) {
    int oy = (int)(ry * openF);
    if (oy < 1) oy = 1;

    HBRUSH scl  = CreateSolidBrush(COL_SCLERA);
    HPEN   edge = CreatePen(PS_SOLID, 1, COL_BUBBLE_EDGE);
    HGDIOBJ ob = SelectObject(dc, scl);
    HGDIOBJ op = SelectObject(dc, edge);
    Ellipse(dc, cx - rx, cy - oy, cx + rx, cy + oy);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(scl); DeleteObject(edge);

    if (openF > 0.35) {
        int ir = (int)(ry * 0.80);
        int px = cx + glanceX;
        /* darker brown outer rim */
        HBRUSH rim = CreateSolidBrush(COL_IRIS_RIM);
        HGDIOBJ ob2 = SelectObject(dc, rim);
        HGDIOBJ op2 = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, px - ir, cy - ir, px + ir, cy + ir);
        /* honey-brown iris */
        int ir2 = (int)(ir * 0.78);
        HBRUSH iris = CreateSolidBrush(COL_IRIS);
        SelectObject(dc, iris);
        Ellipse(dc, px - ir2, cy - ir2, px + ir2, cy + ir2);
        /* pupil */
        int pr = (int)(ry * 0.40);
        HBRUSH pup = CreateSolidBrush(COL_PUPIL);
        SelectObject(dc, pup);
        Ellipse(dc, px - pr, cy - pr, px + pr, cy + pr);
        /* catch-light */
        int hr = (int)(ry * 0.16);
        if (hr < 1) hr = 1;
        HBRUSH hl = CreateSolidBrush(RGB(255, 255, 255));
        SelectObject(dc, hl);
        Ellipse(dc, px - pr, cy - pr, px - pr + hr*2, cy - pr + hr*2);

        /* Restore originals BEFORE deleting, else the still-selected brushes
         * won't actually be freed (per-frame GDI leak). */
        SelectObject(dc, ob2);
        SelectObject(dc, op2);
        DeleteObject(rim);
        DeleteObject(iris);
        DeleteObject(pup);
        DeleteObject(hl);
    }
}

/* Render the overlay content into the shared low-res buffer, then blit it
 * scaled into the given window. */
static void PaintOverlay(HWND hwnd, HDC dst) {
    Overlay *ov = OvFromHwnd(hwnd);
    if (!ov) return;
    EnsureOverlayResources();
    HDC dc = g_loDC;

    HBRUSH key = CreateSolidBrush(COLORKEY);
    RECT full = { 0, 0, LOW, LOH };
    FillRect(dc, &full, key);
    DeleteObject(key);

    SetBkMode(dc, TRANSPARENT);

    /* drop shadow + bubble */
    FillRoundRect(dc, 4, 5, LOW - 2, LOH - 1, 9, RGB(10, 12, 20), RGB(10, 12, 20));
    FillRoundRect(dc, 2, 2, LOW - 4, LOH - 3, 9, COL_BUBBLE, COL_BUBBLE_EDGE);

    ULONGLONG now = GetTickCount64();
    double tsec = now / 1000.0;

    /* blink: quick close every ~3 s */
    double cycle = fmod(tsec, 3.0);
    double openF = 1.0;
    if (cycle < 0.16) {
        double b = cycle / 0.16;
        openF = fabs(cos(b * 3.14159265));
    }
    int glance = (int)lround(2.0 * sin(tsec * 1.1));

    DrawEye(dc, 26, 28, 16, 14, openF, glance);

    int tx = 50;
    SetTextColor(dc, COL_TEXT);
    SelectObject(dc, g_fHead);
    TextOutW(dc, tx, 6, L"LOOK AWAY", 9);

    SetTextColor(dc, COL_TEXT_DIM);
    SelectObject(dc, g_fBody);
    TextOutW(dc, tx, 25, L"Look ~20 ft away", 16);

    int remain = 0;
    if (g_phase == PH_HOLD) {
        LONGLONG ms = (LONGLONG)g_breakEnd - (LONGLONG)now;
        remain = (int)((ms + 999) / 1000);
        if (remain < 0) remain = 0;
    } else if (g_phase == PH_SLIDE_IN) {
        remain = g_cfg.breakSeconds;
    }

    /* progress bar */
    int barL = tx, barR = LOW - 10, barY = 37, barH = 4;
    HBRUSH track = CreateSolidBrush(COL_TRACK);
    RECT rt = { barL, barY, barR, barY + barH };
    FillRect(dc, &rt, track);
    DeleteObject(track);
    double frac = g_cfg.breakSeconds > 0
                  ? (double)remain / (double)g_cfg.breakSeconds : 0.0;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    int fillW = (int)((barR - barL) * frac);
    if (fillW > 0) {
        HBRUSH fb = CreateSolidBrush(COL_ACCENT);
        RECT rf = { barL, barY, barL + fillW, barY + barH };
        FillRect(dc, &rf, fb);
        DeleteObject(fb);
    }

    /* countdown number */
    wchar_t num[16];
    swprintf(num, ARRAYSIZE(num), L"%ds", remain);
    SetTextColor(dc, COL_ACCENT);
    SelectObject(dc, g_fNum);
    TextOutW(dc, tx, 42, num, (int)wcslen(num));

    /* buttons */
    SelectObject(dc, g_fBtn);
    SIZE sz1, sz2;
    const wchar_t *lblSkip = L"Skip";
    wchar_t lblSnz[24];
    swprintf(lblSnz, ARRAYSIZE(lblSnz), L"Snooze %dm", g_cfg.snoozeMinutes);
    GetTextExtentPoint32W(dc, lblSkip, (int)wcslen(lblSkip), &sz1);
    GetTextExtentPoint32W(dc, lblSnz,  (int)wcslen(lblSnz),  &sz2);

    int padX = 5, btnH = 13, gap = 4;
    int wSnz  = sz2.cx + padX * 2;
    int wSkip = sz1.cx + padX * 2;
    int by = LOH - btnH - 3;
    int snzL = LOW - 8 - wSnz;
    int skpL = snzL - gap - wSkip;

    COLORREF skipBg = (ov->hot == 1) ? COL_BTN_HOT : COL_BTN;
    COLORREF snzBg  = (ov->hot == 2) ? COL_BTN_HOT : COL_BTN;
    FillRoundRect(dc, skpL, by, skpL + wSkip, by + btnH, 5, skipBg, COL_BUBBLE_EDGE);
    FillRoundRect(dc, snzL, by, snzL + wSnz,  by + btnH, 5, snzBg,  COL_ACCENT_DIM);

    SetTextColor(dc, COL_TEXT);
    TextOutW(dc, skpL + padX, by + (btnH - sz1.cy) / 2, lblSkip, (int)wcslen(lblSkip));
    TextOutW(dc, snzL + padX, by + (btnH - sz2.cy) / 2, lblSnz,  (int)wcslen(lblSnz));

    /* button hit rects in THIS window's client coords */
    ov->rcSkip.left   = (int)(skpL * ov->scaleX);
    ov->rcSkip.right  = (int)((skpL + wSkip) * ov->scaleX);
    ov->rcSkip.top    = (int)(by * ov->scaleY);
    ov->rcSkip.bottom = (int)((by + btnH) * ov->scaleY);
    ov->rcSnooze.left   = (int)(snzL * ov->scaleX);
    ov->rcSnooze.right  = (int)((snzL + wSnz) * ov->scaleX);
    ov->rcSnooze.top    = (int)(by * ov->scaleY);
    ov->rcSnooze.bottom = (int)((by + btnH) * ov->scaleY);

    SetStretchBltMode(dst, COLORONCOLOR);
    StretchBlt(dst, 0, 0, ov->winW, ov->winH, dc, 0, 0, LOW, LOH, SRCCOPY);
}

/* ------------------------------------------------------------------ */
/* Break lifecycle                                                     */
/* ------------------------------------------------------------------ */

static void StartBreak(void) {
    if (g_breakActive) return;

    EnsureOverlayResources();

    /* Enumerate the target monitors first (so each overlay can see its
     * neighbours for the "facing" layout). */
    g_monCount = 0;
    if (g_cfg.allMonitors) {
        EnumDisplayMonitors(NULL, NULL, CollectMon, 0);
    } else {
        POINT pt; GetCursorPos(&pt);
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi; mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi)) {
            g_monHandle[0] = mon; g_monRect[0] = mi.rcMonitor; g_monCount = 1;
        }
    }
    if (g_monCount == 0) return;

    g_ovCount = 0;
    for (int i = 0; i < g_monCount && g_ovCount < MAX_OVERLAYS; ++i) {
        Overlay *ov = &g_ov[g_ovCount];
        ZeroMemory(ov, sizeof(*ov));
        PlaceOverlayOnMonitor(ov, g_monHandle[i], g_monRect[i]);
        ov->hwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            APP_CLASS_OVERLAY, L"Look20 Break", WS_POPUP,
            ov->offscreen.left, ov->offscreen.top, ov->winW, ov->winH,
            NULL, NULL, g_hInst, NULL);
        if (!ov->hwnd) continue;
        SetLayeredWindowAttributes(ov->hwnd, COLORKEY, WIN_ALPHA, LWA_COLORKEY | LWA_ALPHA);
        g_ovCount++;
    }
    if (g_ovCount == 0) return;   /* nothing created -> stay idle */

    g_breakActive = TRUE;
    g_phase      = PH_SLIDE_IN;
    g_phaseStart = GetTickCount64();
    g_prevTick   = g_phaseStart;

    for (int i = 0; i < g_ovCount; ++i)
        SetWindowPos(g_ov[i].hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);

    SetTimer(g_hMain, TIMER_ANIM, ANIM_TICK_MS, NULL);
    PlayChime(TRUE);
    UpdateTrayTip();
}

static void BeginSlideOut(int result) {
    if (g_phase == PH_SLIDE_OUT) return;

    /* slide each overlay out from wherever it currently rests (may be
     * mid-slide-in, or dragged elsewhere), toward its monitor's nearest edge */
    for (int i = 0; i < g_ovCount; ++i) {
        Overlay *ov = &g_ov[i];
        RECT wr; GetWindowRect(ov->hwnd, &wr);
        ov->target = wr;
        ov->slideOrigin = wr;
        ComputeOffscreen(ov, MonitorWorkArea(
            MonitorFromRect(&wr, MONITOR_DEFAULTTONEAREST)));
    }
    g_breakResult = result;
    g_phase = PH_SLIDE_OUT;
    g_phaseStart = GetTickCount64();
}

static void FinishBreak(void) {
    KillTimer(g_hMain, TIMER_ANIM);
    for (int i = 0; i < g_ovCount; ++i)
        if (g_ov[i].hwnd) DestroyWindow(g_ov[i].hwnd);
    g_ovCount = 0;
    g_breakActive = FALSE;

    if (g_breakResult == END_SNOOZE)
        g_secsLeft = g_cfg.snoozeMinutes * 60;
    else
        ResetCountdown();          /* DONE or SKIP -> fresh cycle */

    UpdateTrayTip();
}

/* During a slide, clip the window to its own monitor so the part that hangs
 * over a neighbouring monitor is invisible - the overlay then appears to slide
 * out from / retract behind the shared seam, never showing on the neighbour. */
static void ClipOverlayToHome(Overlay *ov) {
    RECT w; GetWindowRect(ov->hwnd, &w);
    RECT inter;
    HRGN rgn;
    if (IntersectRect(&inter, &w, &ov->homeMon))
        rgn = CreateRectRgn(inter.left - w.left, inter.top - w.top,
                            inter.right - w.left, inter.bottom - w.top);
    else
        rgn = CreateRectRgn(0, 0, 0, 0);   /* fully off its monitor -> hidden */
    SetWindowRgn(ov->hwnd, rgn, TRUE);     /* window takes ownership of rgn */
}

static void OverlayTick(void) {
    if (!g_breakActive) { KillTimer(g_hMain, TIMER_ANIM); return; }

    ULONGLONG now = GetTickCount64();
    ULONGLONG dt  = now - g_prevTick;
    g_prevTick = now;

    if (g_phase == PH_SLIDE_IN) {
        double t = (now - g_phaseStart) / SLIDE_MS;
        if (t >= 1.0) {
            for (int i = 0; i < g_ovCount; ++i) {
                Overlay *ov = &g_ov[i];
                MoveWindow(ov->hwnd, ov->target.left, ov->target.top,
                           ov->winW, ov->winH, FALSE);
                SetWindowRgn(ov->hwnd, NULL, TRUE);   /* fully resting -> unclip */
            }
            g_phase    = PH_HOLD;
            g_breakEnd = now + (ULONGLONG)g_cfg.breakSeconds * 1000ULL;
        } else {
            double e = easeOut(t);
            for (int i = 0; i < g_ovCount; ++i) {
                Overlay *ov = &g_ov[i];
                int x = ov->offscreen.left + (int)((ov->target.left - ov->offscreen.left) * e);
                int y = ov->offscreen.top  + (int)((ov->target.top  - ov->offscreen.top ) * e);
                MoveWindow(ov->hwnd, x, y, ov->winW, ov->winH, FALSE);
                ClipOverlayToHome(ov);
            }
        }
    } else if (g_phase == PH_HOLD) {
        BOOL anyDragging = FALSE;
        for (int i = 0; i < g_ovCount; ++i)
            if (g_ov[i].dragging) { anyDragging = TRUE; break; }
        if (anyDragging) {
            g_breakEnd += dt;   /* freeze countdown while repositioning */
        } else if (now >= g_breakEnd) {
            PlayChime(FALSE);
            BeginSlideOut(END_DONE);
        }
    } else { /* PH_SLIDE_OUT */
        double t = (now - g_phaseStart) / SLIDE_MS;
        if (t >= 1.0) {
            FinishBreak();
            return;
        }
        double e = easeOut(t);
        for (int i = 0; i < g_ovCount; ++i) {
            Overlay *ov = &g_ov[i];
            int x = ov->slideOrigin.left + (int)((ov->offscreen.left - ov->slideOrigin.left) * e);
            int y = ov->slideOrigin.top  + (int)((ov->offscreen.top  - ov->slideOrigin.top ) * e);
            MoveWindow(ov->hwnd, x, y, ov->winW, ov->winH, FALSE);
            ClipOverlayToHome(ov);
        }
    }

    for (int i = 0; i < g_ovCount; ++i)
        InvalidateRect(g_ov[i].hwnd, NULL, FALSE);
}

/* ------------------------------------------------------------------ */
/* Overlay window procedure                                            */
/* ------------------------------------------------------------------ */

static int HitButton(Overlay *ov, int x, int y) {
    POINT p = { x, y };
    if (PtInRect(&ov->rcSkip, p))   return 1;
    if (PtInRect(&ov->rcSnooze, p)) return 2;
    return 0;
}

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        PaintOverlay(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DPICHANGED: {
        /* Only honour DPI changes from a real user drag across monitors
         * (PH_HOLD). During the slide animation the window stays on its own
         * monitor, so a DPI change there would be spurious. */
        Overlay *ov = OvFromHwnd(hwnd);
        if (ov && g_phase == PH_HOLD) {
            RECT *r = (RECT*)lp;
            ov->winW = r->right - r->left;
            ov->winH = r->bottom - r->top;
            ov->scaleX = (double)ov->winW / LOW;
            ov->scaleY = (double)ov->winH / LOH;
            SetRect(&ov->target, r->left, r->top, r->right, r->bottom);
            SetWindowPos(hwnd, NULL, r->left, r->top, ov->winW, ov->winH,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        return TRUE;

    case WM_MOUSEMOVE: {
        Overlay *ov = OvFromHwnd(hwnd);
        if (!ov) return 0;
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (ov->dragging) {
            POINT pt; GetCursorPos(&pt);
            int nx = pt.x - ov->dragGrab.x;
            int ny = pt.y - ov->dragGrab.y;
            SetWindowPos(hwnd, NULL, nx, ny, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            SetRect(&ov->target, nx, ny, nx + ov->winW, ny + ov->winH);
        } else {
            int h = HitButton(ov, x, y);
            if (h != ov->hot) { ov->hot = h; InvalidateRect(hwnd, NULL, FALSE); }
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        Overlay *ov = OvFromHwnd(hwnd);
        if (!ov) return 0;
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (HitButton(ov, x, y) == 0 && g_phase == PH_HOLD) {
            ov->dragging = TRUE;
            POINT pt; GetCursorPos(&pt);
            RECT wr; GetWindowRect(hwnd, &wr);
            ov->dragGrab.x = pt.x - wr.left;
            ov->dragGrab.y = pt.y - wr.top;
            SetCapture(hwnd);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        Overlay *ov = OvFromHwnd(hwnd);
        if (!ov) return 0;
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (ov->dragging) {
            ov->dragging = FALSE;
            ReleaseCapture();
            /* save position as an offset from this monitor's work-area corner,
             * so it applies consistently across monitors */
            RECT wr; GetWindowRect(hwnd, &wr);
            RECT wa = MonitorWorkArea(MonitorFromRect(&wr, MONITOR_DEFAULTTONEAREST));
            g_cfg.customX = wr.left - wa.left;
            g_cfg.customY = wr.top  - wa.top;
            g_cfg.position = POS_CUSTOM;
            SaveConfig();
        } else {
            int h = HitButton(ov, x, y);
            if (h == 1) BeginSlideOut(END_SKIP);
            else if (h == 2) BeginSlideOut(END_SNOOZE);
        }
        return 0;
    }

    case WM_DESTROY:
        /* shared resources are freed at app shutdown, not per overlay */
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* Menu command handling                                               */
/* ------------------------------------------------------------------ */

static void OnCommand(int id) {
    if (id >= IDM_WORK_BASE && id < IDM_WORK_BASE + 1000) {
        g_cfg.workMinutes = id - IDM_WORK_BASE;
        if (!g_breakActive && !g_paused) ResetCountdown();
        SaveConfig(); UpdateTrayTip(); return;
    }
    if (id >= IDM_BREAK_BASE && id < IDM_BREAK_BASE + 1000) {
        g_cfg.breakSeconds = id - IDM_BREAK_BASE;
        SaveConfig(); return;
    }
    if (id >= IDM_SNOOZE_BASE && id < IDM_SNOOZE_BASE + 1000) {
        g_cfg.snoozeMinutes = id - IDM_SNOOZE_BASE;
        SaveConfig(); return;
    }
    if (id >= IDM_POS_BASE && id < IDM_POS_BASE + POS_COUNT) {
        g_cfg.position = id - IDM_POS_BASE;
        SaveConfig(); return;
    }

    switch (id) {
    case IDM_BREAK_NOW:
        StartBreak();
        break;
    case IDM_PAUSE:
        g_paused = !g_paused;   /* g_secsLeft stays frozen; resume continues it */
        UpdateTrayTip();
        break;
    case IDM_RESET:
        ResetCountdown();
        UpdateTrayTip();
        break;
    case IDM_ALL_MONITORS:
        g_cfg.allMonitors = !g_cfg.allMonitors; SaveConfig(); break;
    case IDM_SOUND:
        g_cfg.sound = !g_cfg.sound; SaveConfig(); break;
    case IDM_PAUSE_IDLE:
        g_cfg.pauseWhenIdle = !g_cfg.pauseWhenIdle; SaveConfig(); break;
    case IDM_SKIP_FULLSCREEN:
        g_cfg.skipFullscreen = !g_cfg.skipFullscreen; SaveConfig(); break;
    case IDM_AUTOSTART:
        g_cfg.autostart = !g_cfg.autostart;
        ApplyAutostart(g_cfg.autostart);
        SaveConfig();
        break;
    case IDM_ABOUT:
        MessageBoxW(NULL,
            L"Look20 - 20-20-20 eye-break reminder\n\n"
            L"Every 20 minutes, look at something about 20 feet (6 m) away "
            L"for 20 seconds to relax your eyes.\n\n"
            L"Left-click the tray eye to take a break right now.\n"
            L"Right-click it for settings.\n"
            L"Drag the overlay to set a custom position.\n\n"
            L"Version 1.0",
            L"About Look20", MB_OK | MB_ICONINFORMATION);
        break;
    case IDM_EXIT:
        DestroyWindow(g_hMain);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Main (hidden) window procedure                                      */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static UINT s_taskbarCreated = 0;

    if (msg == s_taskbarCreated && s_taskbarCreated != 0) {
        AddTray();           /* Explorer restarted -> re-add the tray icon */
        UpdateTrayTip();
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        s_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        return 0;

    case WM_TIMER:
        if (wp == TIMER_ANIM) {
            OverlayTick();
        } else if (wp == TIMER_WORK) {
            if (g_breakActive || g_paused) return 0;
            if (g_cfg.pauseWhenIdle && IdleMillis() >= IDLE_THRESHOLD_MS)
                return 0;
            if (g_secsLeft > 0) g_secsLeft--;
            if (g_secsLeft <= 0) {
                if (g_cfg.skipFullscreen && ForegroundIsFullscreen()) {
                    g_secsLeft = FULLSCREEN_RETRY_S;
                } else {
                    StartBreak();
                    return 0;
                }
            }
            UpdateTrayTip();
        }
        return 0;

    case WM_TRAYICON:
        switch (LOWORD(lp)) {
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowMenu();          /* right-click = settings menu */
            break;
        case WM_LBUTTONUP:
            StartBreak();        /* left-click  = take a break now */
            break;
        }
        return 0;

    case WM_COMMAND:
        OnCommand(LOWORD(wp));
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_WORK);
        KillTimer(hwnd, TIMER_ANIM);
        for (int i = 0; i < g_ovCount; ++i)
            if (g_ov[i].hwnd) DestroyWindow(g_ov[i].hwnd);
        g_ovCount = 0;
        FreeOverlayResources();
        RemoveTray();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

static void ResolveDpiHelper(void) {
    HMODULE sh = LoadLibraryW(L"shcore.dll");
    if (sh)
        p_GetDpiForMonitor = (PFN_GetDpiForMonitor)(void(*)(void))
            GetProcAddress(sh, "GetDpiForMonitor");
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR cmd, int show) {
    (void)hPrev; (void)cmd; (void)show;
    g_hInst = hInst;

    HANDLE mtx = CreateMutexW(NULL, FALSE, APP_MUTEX);
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"Look20 is already running (check the system tray).",
                    APP_NAME, MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    ResolveDpiHelper();
    LoadConfig();
    ApplyAutostart(g_cfg.autostart);   /* keep Run key in sync */

    HICON appIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = MainWndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = APP_CLASS_MAIN;
    wc.hIcon         = appIcon;
    RegisterClassExW(&wc);

    WNDCLASSEXW wo;
    ZeroMemory(&wo, sizeof(wo));
    wo.cbSize        = sizeof(wo);
    wo.lpfnWndProc   = OverlayWndProc;
    wo.hInstance     = hInst;
    wo.lpszClassName = APP_CLASS_OVERLAY;
    wo.hCursor       = LoadCursor(NULL, IDC_ARROW);
    RegisterClassExW(&wo);

    /* Hidden top-level window (NOT message-only) that owns the tray icon and
     * the timers. Top-level so it receives the broadcast "TaskbarCreated"
     * message and can become foreground for the tray menu. WS_EX_TOOLWINDOW
     * keeps it off the taskbar / Alt-Tab; it is never shown. */
    g_hMain = CreateWindowExW(WS_EX_TOOLWINDOW, APP_CLASS_MAIN, APP_NAME,
                              WS_POPUP, 0, 0, 0, 0, NULL, NULL, hInst, NULL);
    if (!g_hMain) return 1;

    AddTray();
    ResetCountdown();
    UpdateTrayTip();
    SetTimer(g_hMain, TIMER_WORK, WORK_TICK_MS, NULL);

    ShowBalloon(L"Look20 is running",
                L"I'll remind you to rest your eyes. Left-click the tray eye for a "
                L"break now, right-click for settings.");

    MSG m = {0};
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    if (mtx) CloseHandle(mtx);
    return (int)m.wParam;
}
