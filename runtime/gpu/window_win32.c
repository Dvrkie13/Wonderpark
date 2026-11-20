/* From MildRain479/PokePark-Wiicompiled recomp/gpu/window_win32.c (CC0). */
/* The game's window, which a device (gx_device.h) shows its pictures in.
 *
 * Environment:
 *   RECOMP_GX_HIDDEN=1         do not show the window */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "gx_device.h"
#include "host_settings.h"

static HWND sWnd;
static int sHidden, sFullscreen;
/* EFB pixels to screen pixels. In 16:9 the game squeezes a wide picture
 * into the EFB's 640 columns for the TV to stretch out: its cameras
 * project a 16:9 picture onto the 640x456 it shows. So it is shown at
 * 16:9. (Shown 4/3 as wide, 1.87:1, it was 5% too wide, with bars above
 * and below.) */
static double sPixelAspect = 1;
static WINDOWPLACEMENT sWindowed;
/* From another thread (the render thread), the title waits for the
 * window's own thread: setting it there would wait until that thread
 * handles its messages. */
static DWORD sThread;
static CRITICAL_SECTION sTitleLock;
static char sTitle[128];
static volatile LONG sTitlePending;


/* Fullscreen (window mode 2): the screen switched to the Resolution
 * setting's size while the game is in front (Match screen leaves it, which
 * is then borderless). sModeDevice is the display changed, "" if none. */
static char sModeDevice[CCHDEVICENAME];

static void restore_mode(void) {
    if (!sModeDevice[0]) return;
    ChangeDisplaySettingsExA(sModeDevice, NULL, NULL, 0, NULL);
    sModeDevice[0] = 0;
}

/* ---- Screens (Display > Screen): the main one first, then the others in
 * Windows' order. */
#define MAX_SCREENS 16
typedef struct {
    HMONITOR h[MAX_SCREENS];
    int n;
} ScreenList;

static BOOL CALLBACK add_screen(HMONITOR m, HDC dc, LPRECT r, LPARAM lp) {
    ScreenList* l = (ScreenList*)lp;
    MONITORINFO mi;
    (void)dc, (void)r;
    if (l->n >= MAX_SCREENS) return FALSE;
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoA(m, &mi) && (mi.dwFlags & MONITORINFOF_PRIMARY) && l->n > 0) {
        memmove(l->h + 1, l->h, sizeof(HMONITOR) * (size_t)l->n);
        l->h[0] = m;
    } else {
        l->h[l->n] = m;
    }
    l->n++;
    return TRUE;
}

static ScreenList screens(void) {
    ScreenList l;
    l.n = 0;
    EnumDisplayMonitors(NULL, NULL, add_screen, (LPARAM)&l);
    return l;
}

int win_screen_count(void) { return screens().n; }

/* The screen the game goes to: the one chosen, else the window's own. */
static HMONITOR target_monitor(void) {
    int want = g_x[X_MONITOR];
    if (want > 0) {
        ScreenList l = screens();
        if (want <= l.n) return l.h[want - 1];
    }
    return MonitorFromWindow(sWnd, MONITOR_DEFAULTTOPRIMARY);
}

/* The refresh rates the chosen screen offers at the size fullscreen uses
 * (w x h; 0: its desktop's), lowest first; how many. */
int win_refresh_rates(int w, int h, int* out, int max) {
    MONITORINFOEXA mi;
    DEVMODEA dm;
    DWORD i;
    int n = 0, k;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(target_monitor(), (MONITORINFO*)&mi)) return 0;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (!w && EnumDisplaySettingsA(mi.szDevice, ENUM_REGISTRY_SETTINGS, &dm)) w = (int)dm.dmPelsWidth, h = (int)dm.dmPelsHeight;
    for (i = 0; EnumDisplaySettingsA(mi.szDevice, i, &dm); i++) {
        int hz = (int)dm.dmDisplayFrequency;
        if ((int)dm.dmPelsWidth != w || (int)dm.dmPelsHeight != h || dm.dmBitsPerPel < 24 || hz < 50) continue;
        for (k = 0; k < n && out[k] != hz; k++) {}
        if (k < n || n >= max) continue;
        for (k = n; k > 0 && out[k - 1] > hz; k--) out[k] = out[k - 1];
        out[k] = hz;
        n++;
    }
    return n;
}

/* Switches the chosen screen to w x h (at hz, if not 0), or back when w is
 * 0; 1 if it's at that mode now. */
static int set_mode(int w, int h, int hz) {
    MONITORINFOEXA mi;
    DEVMODEA dm;
    if (!w) {
        restore_mode();
        return 1;
    }
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(target_monitor(), (MONITORINFO*)&mi)) return 0;
    if (sModeDevice[0] && strcmp(sModeDevice, mi.szDevice)) restore_mode(); /* another screen chosen */
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && (int)dm.dmPelsWidth == w &&
        (int)dm.dmPelsHeight == h && (!hz || (int)dm.dmDisplayFrequency == hz))
        return 1;
    dm.dmPelsWidth = (DWORD)w;
    dm.dmPelsHeight = (DWORD)h;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    if (hz) dm.dmDisplayFrequency = (DWORD)hz, dm.dmFields |= DM_DISPLAYFREQUENCY;
    if (ChangeDisplaySettingsExA(mi.szDevice, &dm, NULL, CDS_FULLSCREEN, NULL) != DISP_CHANGE_SUCCESSFUL) {
        dm.dmFields &= ~(DWORD)DM_DISPLAYFREQUENCY;
        if (!hz || ChangeDisplaySettingsExA(mi.szDevice, &dm, NULL, CDS_FULLSCREEN, NULL) != DISP_CHANGE_SUCCESSFUL) {
            fprintf(stderr, "window: the screen can't be set to %dx%d; fullscreen stays at its own size\n", w, h);
            return 0;
        }
        fprintf(stderr, "window: the screen can't run at %d Hz at %dx%d; it keeps its own rate\n", hz, w, h);
    }
    if (EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
        fprintf(stderr, "window: fullscreen: the screen is at %lux%lu, %lu Hz\n", (unsigned long)dm.dmPelsWidth,
                (unsigned long)dm.dmPelsHeight, (unsigned long)dm.dmDisplayFrequency);
    if (!sModeDevice[0]) {
        static int registered;
        if (!registered) registered = 1, atexit(restore_mode);
    }
    memcpy(sModeDevice, mi.szDevice, sizeof(sModeDevice));
    return 1;
}

static void cover_monitor(void) {
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoA(target_monitor(), &mi);
    SetWindowPos(sWnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
}

static const int kSizes[4][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}};

/* The display mode fullscreen wants: the resolution's (Match screen: the
 * desktop's size) at the refresh rate chosen, while in front. */
static void apply_mode(int active) {
    int r = g_x[X_RESOLUTION], hz = g_x[X_REFRESH_RATE];
    if (sFullscreen && g_x[X_WINDOW_MODE] == 2 && active && ((r >= 0 && r < 4) || hz)) {
        int w = 0, h = 0;
        if (r >= 0 && r < 4) {
            w = kSizes[r][0], h = kSizes[r][1];
        } else {
            MONITORINFOEXA mi;
            DEVMODEA dm;
            mi.cbSize = sizeof(mi);
            memset(&dm, 0, sizeof(dm));
            dm.dmSize = sizeof(dm);
            if (GetMonitorInfoA(target_monitor(), (MONITORINFO*)&mi) &&
                EnumDisplaySettingsA(mi.szDevice, ENUM_REGISTRY_SETTINGS, &dm))
                w = (int)dm.dmPelsWidth, h = (int)dm.dmPelsHeight;
        }
        if (w && set_mode(w, h, hz)) cover_monitor();
    } else if (sModeDevice[0]) {
        set_mode(0, 0, 0);
        if (sFullscreen) cover_monitor();
    } else if (sFullscreen) {
        cover_monitor(); /* (another screen chosen) */
    }
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLOSE) {
        restore_mode();
        exit(0);
    }
    if (msg == WM_ACTIVATEAPP && sFullscreen && g_x[X_WINDOW_MODE] == 2) {
        /* Alt+Tab out of fullscreen: the desktop gets its own size back,
         * and the game's window steps aside until it's chosen again. */
        apply_mode(wp != 0);
        if (!wp && !sHidden) ShowWindow(sWnd, SW_MINIMIZE);
        else if (wp && IsIconic(sWnd)) ShowWindow(sWnd, SW_RESTORE);
    }
    /* Alt+Enter toggles fullscreen; Alt alone doesn't open the window
     * menu, which would hold the game's keys. */
    if (msg == WM_SYSKEYDOWN && wp == VK_RETURN && (lp & (1 << 29))) {
        if (!(lp & (1 << 30))) settings_set_x(X_WINDOW_MODE, g_settings.fullscreen ? 0 : 1);
        return 0;
    }
    if (msg == WM_SYSCHAR || (msg == WM_SYSCOMMAND && (wp & 0xFFF0) == SC_KEYMENU)) return 0;
    if (menu_window_message(h, msg, wp, lp)) return 0;
    return DefWindowProcA(h, msg, wp, lp);
}

void win_create(void) {
    WNDCLASSA wc;
    RECT r = {0, 0, 640 * 2, 456 * 2}; /* the 456 lines the game shows, so the picture fills it */
    const char* v;
    extern int hle_widescreen(void);
    if (hle_widescreen()) {
        sPixelAspect = 16.0 / 9.0 * 456.0 / 640.0;
        r.right = (LONG)(r.right * sPixelAspect + 0.5);
    }
    if ((v = getenv("RECOMP_WINDOW_SIZE")) != NULL) {
        /* Tests: the window's inside, WxH (hidden windows keep it). */
        int ww, hh;
        if (sscanf(v, "%dx%d", &ww, &hh) == 2 && ww > 0 && hh > 0) r.right = ww, r.bottom = hh;
    }
    sThread = GetCurrentThreadId();
    InitializeCriticalSection(&sTitleLock);
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "Wonderpark";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    /* The app icon (runtime/host/wonderpark.rc, id 1) for the title bar,
     * taskbar and Alt+Tab. */
    wc.hIcon = (HICON)LoadImageA(wc.hInstance, MAKEINTRESOURCEA(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                 GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    RegisterClassA(&wc);
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    sWnd = CreateWindowA("Wonderpark", "Wonderpark", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                         r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    {
        HICON sm = (HICON)LoadImageA(wc.hInstance, MAKEINTRESOURCEA(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                     GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
        if (sm) SendMessageA(sWnd, WM_SETICON, ICON_SMALL, (LPARAM)sm);
    }
    v = getenv("RECOMP_GX_HIDDEN");
    {
        extern int g_wp_headless; /* --headless (main.c) */
        sHidden = (v && *v && *v != '0') || g_wp_headless;
    }
    if (!sHidden) {
        ShowWindow(sWnd, SW_SHOW);
        settings_apply_window();
    }
}

void* win_handle(void) { return sWnd; }
int win_hidden(void) { return sHidden; }
double win_pixel_aspect(void) { return sPixelAspect; }

void win_size(int* w, int* h) {
    RECT r;
    GetClientRect(sWnd, &r);
    *w = r.right - r.left;
    *h = r.bottom - r.top;
}

void win_set_fullscreen(int on) {
    DWORD style = (DWORD)GetWindowLongA(sWnd, GWL_STYLE);
    if (sHidden) return;
    if (on == sFullscreen) {
        if (on) apply_mode(1); /* fullscreen kind or resolution changed */
        return;
    }
    sFullscreen = on;
    if (on) {
        sWindowed.length = sizeof(sWindowed);
        GetWindowPlacement(sWnd, &sWindowed);
        SetWindowLongA(sWnd, GWL_STYLE, (LONG)((style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP));
        cover_monitor();
        apply_mode(1);
    } else {
        apply_mode(0);
        SetWindowLongA(sWnd, GWL_STYLE, (LONG)((style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW));
        SetWindowPlacement(sWnd, &sWindowed);
        SetWindowPos(sWnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

/* The window mode and resolution settings (host_settings.h X_*): in a
 * window, its client area is that size (Match screen: maximized); in
 * borderless fullscreen it covers the monitor (win_set_fullscreen). */
void settings_apply_window(void) {
    int r = g_x[X_RESOLUTION];
    if (!sWnd || sHidden) return;
    if (GetCurrentThreadId() != sThread) return; /* only from the window's thread */
    win_set_fullscreen(g_settings.fullscreen);
    if (g_settings.fullscreen) return;
    if (r >= 0 && r < 4) {
        RECT rc = {0, 0, kSizes[r][0], kSizes[r][1]};
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        ShowWindow(sWnd, SW_RESTORE);
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        GetMonitorInfoA(target_monitor(), &mi);
        SetWindowPos(sWnd, NULL, mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - (rc.right - rc.left)) / 2,
                     mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - (rc.bottom - rc.top)) / 2,
                     rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOOWNERZORDER);
    } else {
        if (g_x[X_MONITOR] > 0) {
            /* Onto the chosen screen first, so it's that one that's filled. */
            MONITORINFO mi;
            mi.cbSize = sizeof(mi);
            ShowWindow(sWnd, SW_RESTORE);
            GetMonitorInfoA(target_monitor(), &mi);
            SetWindowPos(sWnd, NULL, mi.rcWork.left + 40, mi.rcWork.top + 40, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
        }
        ShowWindow(sWnd, SW_MAXIMIZE);
    }
}

/* Whether the game's window is in front (pause and mute in the background). */
int win_has_focus(void) {
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (!w) return 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

void win_set_title(const char* title) {
    if (GetCurrentThreadId() == sThread) {
        SetWindowTextA(sWnd, title);
        return;
    }
    EnterCriticalSection(&sTitleLock);
    snprintf(sTitle, sizeof(sTitle), "%s", title);
    sTitlePending = 1;
    LeaveCriticalSection(&sTitleLock);
}

void win_pump(void) {
    MSG msg;
    if (sTitlePending) {
        char t[128];
        EnterCriticalSection(&sTitleLock);
        memcpy(t, sTitle, sizeof(t));
        sTitlePending = 0;
        LeaveCriticalSection(&sTitleLock);
        SetWindowTextA(sWnd, t);
    }
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

/* The compositor's own rate is another monitor's when there are several.
 * Whole numbers just under a multiple of 10 are the NTSC-style rates (119
 * is 119.88 Hz). Asked again every few seconds, as the window can move. */
double win_display_hz(void) {
    static double hz;
    static DWORD checked;
    DWORD now = GetTickCount();
    MONITORINFOEXA mi;
    DEVMODEA dm;
    if (checked && now - checked < 3000) return hz;
    checked = now ? now : 1;
    hz = 0;
    mi.cbSize = sizeof(mi);
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoA(MonitorFromWindow(sWnd, MONITOR_DEFAULTTOPRIMARY), (MONITORINFO*)&mi) &&
        EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) {
        hz = dm.dmDisplayFrequency;
        if (dm.dmDisplayFrequency % 10 == 9) hz = (hz + 1) * 1000.0 / 1001.0;
    }
    return hz;
}

int gxb_display_hz(void) { return (int)(win_display_hz() + 0.5); }
