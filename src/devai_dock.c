/*
 * devai_dock.exe —— 把 DevAI 助手面板嵌进 Dev-C++ 主窗口
 *
 * 用法：
 *   devai_dock.exe --host=DevCpp.exe --panel=DevAI 小助手 --width=440
 *
 * 行为：
 *   - 等待 Dev-C++ 与面板窗口出现，SetParent 成子窗口，去掉标题栏，贴右侧填满高度；
 *   - 每 250ms 跟随 Dev-C++ 的尺寸变化；
 *   - 读 state\dockmode.txt：dock=嵌入，float=还原成独立浮窗；
 *   - 面板被关闭则自动退出。
 */
#define _WIN32_WINNT 0x0601
#define WINVER 0x0601
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

static WCHAR gHostName[64]   = L"DevCpp.exe";
static WCHAR gPanelName[128] = L"DevAI";
static int   gPanelW         = 440;
static WCHAR gHome[MAX_PATH] = L"D:\\devai";
static WCHAR gModeFile[MAX_PATH];
static WCHAR gLogFile[MAX_PATH];
static WCHAR gAliveFile[MAX_PATH];
static WCHAR gRaiseFile[MAX_PATH];

/* ------------------------------------------------------------------ 小工具 */

static BOOL wcontains(const WCHAR *hay, const WCHAR *needle)
{
    if (!hay || !needle || !*needle) return FALSE;
    int n = (int)wcslen(needle);
    for (const WCHAR *p = hay; *p; ++p) {
        int i = 0;
        while (i < n && p[i] &&
               towlower(p[i]) == towlower(needle[i])) i++;
        if (i == n) return TRUE;
    }
    return FALSE;
}

static void logline(const WCHAR *fmt, ...)
{
    if (!gLogFile[0]) return;
    WCHAR buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 1000, fmt, ap);
    va_end(ap);
    buf[1000] = 0;

    char utf8[2048];
    int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, 2046, NULL, NULL);
    if (n <= 1) return;
    utf8[n - 1] = '\r';
    utf8[n] = '\n';

    HANDLE f = CreateFileW(gLogFile, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, utf8, (DWORD)n + 1, &written, NULL);
    CloseHandle(f);
}

/* 面板心跳：让服务端知道窗口还活着，避免重复开窗 */
static void writeAlive(void)
{
    if (!gAliveFile[0]) return;
    HANDLE f = CreateFileW(gAliveFile, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, "1", 1, &written, NULL);
    CloseHandle(f);
}

static void clearAlive(void)
{
    if (gAliveFile[0]) DeleteFileW(gAliveFile);
}

static long readRaise(void)
{
    if (!gRaiseFile[0]) return 0;
    HANDLE f = CreateFileW(gRaiseFile, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    char raw[32] = {0};
    DWORD got = 0;
    ReadFile(f, raw, 31, &got, NULL);
    CloseHandle(f);
    return atol(raw);
}

static void readMode(WCHAR *out, int cap)
{
    wcscpy_s(out, cap, L"dock");
    HANDLE f = CreateFileW(gModeFile, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    char raw[64] = {0};
    DWORD got = 0;
    ReadFile(f, raw, 40, &got, NULL);
    CloseHandle(f);
    if (got >= 5 && (raw[0] == 'f' || raw[0] == 'F')) wcscpy_s(out, cap, L"float");
}

/* 网页上报的缩放比（devicePixelRatio），默认 1 */
static double readDpr(void)
{
    WCHAR path[MAX_PATH];
    wsprintfW(path, L"%s\\state\\dpr.txt", gHome);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 1.0;
    char raw[32] = {0};
    DWORD got = 0;
    ReadFile(f, raw, 31, &got, NULL);
    CloseHandle(f);
    double v = atof(raw);
    return (v > 0.6 && v < 5) ? v : 1.0;
}

/* ------------------------------------------------------------ 找 Dev-C++ 主窗口 */

static HWND gFound;
static long gBestArea;

static BOOL CALLBACK enumHost(HWND h, LPARAM lp)
{
    (void)lp;
    RECT r;
    if (!GetWindowRect(h, &r)) return TRUE;
    int w = r.right - r.left;
    int hh = r.bottom - r.top;
    if (w < 640 || hh < 420) return TRUE;   /* 过滤掉 Delphi 的小工具窗 / IME 窗 */

    WCHAR title[256];
    GetWindowTextW(h, title, 256);
    if (!title[0]) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!ph) ph = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!ph) return TRUE;

    WCHAR path[MAX_PATH] = L"";
    DWORD sz = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameW(ph, 0, path, &sz);
    CloseHandle(ph);
    if (!ok) return TRUE;
    if (!wcontains(path, gHostName)) return TRUE;

    long area = (long)w * (long)hh;
    if (area > gBestArea) {
        gBestArea = area;
        gFound = h;
    }
    return TRUE;
}

static HWND findHost(void)
{
    gFound = NULL;
    gBestArea = 0;
    EnumWindows(enumHost, 0);
    return gFound;
}

/* --------------------------------------------------------------- 找面板窗口 */

static HWND gPanelList[8];
static int  gPanelCount;

static BOOL titleIsPanel(const WCHAR *title)
{
    if (!title || !title[0]) return FALSE;
    const WCHAR *a = title;
    while (*a == L' ') ++a;
    WCHAR tmp[256];
    wcscpy_s(tmp, 256, a);
    int n = (int)wcslen(tmp);
    while (n > 0 && tmp[n - 1] == L' ') tmp[--n] = 0;
    if (_wcsicmp(tmp, gPanelName) == 0) return TRUE;
    /* 允许标题后缀，例如 "DevAI 小助手 - Dev-C++" */
    if (wcslen(gPanelName) > 0 && wcslen(tmp) > wcslen(gPanelName) &&
        _wcsnicmp(tmp, gPanelName, wcslen(gPanelName)) == 0) return TRUE;
    return FALSE;
}

static BOOL CALLBACK enumPanel(HWND h, LPARAM lp)
{
    (void)lp;
    WCHAR title[256];
    GetWindowTextW(h, title, 256);
    if (!titleIsPanel(title)) return TRUE;
    if (gPanelCount < 8) gPanelList[gPanelCount++] = h;
    return TRUE;
}

static BOOL CALLBACK enumPanelChild(HWND h, LPARAM lp)
{
    (void)lp;
    WCHAR title[256];
    GetWindowTextW(h, title, 256);
    if (titleIsPanel(title) && gPanelCount < 8) gPanelList[gPanelCount++] = h;
    return TRUE;
}

/* 同一个进程可能同时存在多个同名窗口（Edge 会多开一个），全部交给 glue 处理 */
static int findPanels(HWND host)
{
    gPanelCount = 0;
    EnumWindows(enumPanel, 0);
    if (gPanelCount == 0 && host && IsWindow(host)) EnumChildWindows(host, enumPanelChild, 0);
    return gPanelCount;
}

/* ------------------------------------------------------------------ 嵌入/还原 */

static void noRound(HWND h)
{
    typedef HRESULT (WINAPI *DwmSetAttr)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;
    DwmSetAttr fn = (DwmSetAttr)GetProcAddress(dwm, "DwmSetWindowAttribute");
    if (fn) {
        DWORD pref = 1; /* DWMWCP_DONOTROUND */
        fn(h, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &pref, sizeof(pref));
        DWORD border = 0xFFFFFFFE; /* DWMWA_COLOR_NONE */
        fn(h, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
    }
}

static int dpiOf(HWND h)
{
    typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
    typedef UINT (WINAPI *GetDpiForSystemFn)(void);
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        /* 面板是 Chromium（per-monitor DPI aware），它的 DPI 才是网页真正用的缩放比 */
        if (h && IsWindow(h)) {
            GetDpiForWindowFn fw = (GetDpiForWindowFn)GetProcAddress(u, "GetDpiForWindow");
            if (fw) {
                UINT d = fw(h);
                if (d >= 72 && d <= 480) return (int)d;
            }
        }
        GetDpiForSystemFn fn = (GetDpiForSystemFn)GetProcAddress(u, "GetDpiForSystem");
        if (fn) {
            UINT d = fn();
            if (d >= 72 && d <= 480) return (int)d;
        }
    }
    HDC dc = GetDC(NULL);
    int d = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(NULL, dc);
    return d ? d : 96;
}

/*
 * 贴靠模式：不改窗口样式（Chromium 去掉标题栏后不再绘制内容），
 * 只让它从任务栏里消失、不抢焦点，靠位置/尺寸贴着 Dev-C++ 右侧。
 */
static void makeDocked(HWND panel, HWND host)
{
    /* 一个样式都不要动：Chromium 窗口被改样式后会停止绘制内容（整块变透明） */
    (void)panel;
    (void)host;
}

/* 抢到前台：Chromium 被遮挡时会停止绘制，整窗变透明，看上去就是"没显示" */
static void bringToFront(HWND panel)
{
    if (!IsWindow(panel)) return;
    if (IsIconic(panel)) ShowWindow(panel, SW_RESTORE);
    ShowWindow(panel, SW_SHOW);

    HWND fg = GetForegroundWindow();
    DWORD fgTid = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
    DWORD myTid = GetCurrentThreadId();
    if (fgTid && fgTid != myTid) AttachThreadInput(myTid, fgTid, TRUE);
    SetForegroundWindow(panel);
    BringWindowToTop(panel);
    if (fgTid && fgTid != myTid) AttachThreadInput(myTid, fgTid, FALSE);
}

/* 浮窗模式：就当普通独立窗口摆放，样式一个都别改
 * （Chromium 窗口被外部改 GWL_STYLE 后会把窗口藏掉/停止绘制，实测直接 vis=False） */
static void makeFloat(HWND panel, HWND host)
{
    /* 浮窗尺寸：440×700 逻辑像素，别占满整屏，能随手拖动 */
    int w = 440, hh = 700;
    RECT scr;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &scr, 0);
    double dpr = readDpr();
    w = (int)(440 * dpr + 0.5);
    hh = (int)(700 * dpr + 0.5);
    if (hh > scr.bottom - scr.top - 40) hh = scr.bottom - scr.top - 40;
    int x = scr.right - w - 20, y = scr.top + 20;
    if (host && IsWindow(host)) {
        RECT hr;
        GetWindowRect(host, &hr);
        x = hr.right - w;
        y = hr.top + (int)(30 * dpr);
        if (x < scr.left) x = scr.left;
        if (y < scr.top) y = scr.top;
        if (y + hh > scr.bottom) y = scr.bottom - hh - 4;
    }
    if (IsIconic(panel)) ShowWindow(panel, SW_RESTORE);
    /* 置顶：Dev-C++ 最大化时会把浮窗整个盖住，被遮挡的 Chromium 会停止绘制 */
    SetWindowPos(panel, HWND_TOPMOST, x, y, w, hh, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    noRound(panel);
    bringToFront(panel);
}

/* 贴在 Dev-C++ 右侧，和它等高，跟随位置/尺寸/最小化 */
static void glue(HWND panel, HWND host)
{
    if (IsIconic(host) || !IsWindowVisible(host)) {
        if (IsWindowVisible(panel)) ShowWindow(panel, SW_HIDE);
        return;
    }
    if (!IsWindowVisible(panel)) ShowWindow(panel, SW_SHOWNOACTIVATE);
    RECT hr;
    if (!GetWindowRect(host, &hr)) return;
    int hw = hr.right - hr.left;
    int hh = hr.bottom - hr.top;
    if (hw <= 0 || hh <= 0) return;

    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);

    /* 宽度按网页缩放比换算：440 逻辑像素 × devicePixelRatio */
    int ideal = (int)(gPanelW * readDpr() + 0.5);

    /* 右侧还有空间就贴在外面，没有就往里压（像 IDE 右侧栏） */
    int room = wa.right - hr.right;
    int w = ideal;
    int x;
    if (room >= ideal) {
        x = hr.right;
    } else {
        /* 右边没地方了：压在 Dev-C++ 右边缘上，当作 IDE 右侧栏 */
        x = hr.right - w;
        if (x < wa.left) x = wa.left;
    }

    int y = hr.top;
    if (y < wa.top) y = wa.top;
    if (y + hh > wa.bottom) hh = wa.bottom - y;

    RECT pr;
    GetWindowRect(panel, &pr);
    if (pr.left != x || pr.top != y || pr.right - pr.left != w || pr.bottom - pr.top != hh) {
        SetWindowPos(panel, NULL, x, y, w, hh, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    /* Dev-C++ 在前台时把面板压在最上层：Chromium 被遮挡后会停止绘制而变透明 */
    HWND fg = GetForegroundWindow();
    int hostFront = (fg == host) || (fg && GetAncestor(fg, GA_ROOT) == host);
    SetWindowPos(panel, hostFront ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

/* --------------------------------------------------------------------- 入口 */

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, PWSTR cmd, int show)
{
    (void)hi; (void)hp; (void)cmd; (void)show;
    SetProcessDPIAware();

    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        if (wcsncmp(argv[i], L"--host=", 7) == 0) wcscpy_s(gHostName, 64, argv[i] + 7);
        else if (wcsncmp(argv[i], L"--panel=", 8) == 0) wcscpy_s(gPanelName, 128, argv[i] + 8);
        else if (wcsncmp(argv[i], L"--width=", 8) == 0) gPanelW = _wtoi(argv[i] + 8);
        else if (wcsncmp(argv[i], L"--home=", 7) == 0) wcscpy_s(gHome, MAX_PATH, argv[i] + 7);
        else if (wcscmp(argv[i], L"--float") == 0) { /* 临时：直接以浮窗模式启动 */ }
    }
    if (argv) LocalFree(argv);

    WCHAR env[MAX_PATH];
    if (GetEnvironmentVariableW(L"DEVAI_HOME", env, MAX_PATH) && env[0])
        wcscpy_s(gHome, MAX_PATH, env);

    wsprintfW(gModeFile, L"%s\\state\\dockmode.txt", gHome);
    wsprintfW(gLogFile, L"%s\\state\\dock.log", gHome);
    wsprintfW(gAliveFile, L"%s\\state\\panel.alive", gHome);
    wsprintfW(gRaiseFile, L"%s\\state\\raise.txt", gHome);

    HANDLE mutex = CreateMutexW(NULL, TRUE, L"DevAI_Dock_SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;
    }

    logline(L"[dock] start host=%s panel=%s width=%d home=%s", gHostName, gPanelName, gPanelW, gHome);

    DWORD started = GetTickCount();
    HWND host = NULL;
    WCHAR lastMode[32] = L"";
    int applied = 0;   /* 0=未设置 1=贴靠 2=浮窗 */
    HWND seen[8] = {0};
    int  seenMode[8] = {0};
    int  everFound = 0;
    DWORD goneAt = 0;
    long raiseSeen = 0;

    for (;;) {
        WCHAR mode[32];
        readMode(mode, 32);

        int n = findPanels(host);
        if (n == 0) {
            /* 之前出现过、现在没了：面板被关掉，跟着退出 */
            if (everFound) {
                if (!goneAt) goneAt = GetTickCount();
                else if (GetTickCount() - goneAt > 20000) {
                    logline(L"[dock] panel closed, exit");
                    break;
                }
            } else if (GetTickCount() - started > 60000) {
                logline(L"[dock] panel never appeared, exit");
                break;
            }
            Sleep(220);
            continue;
        }
        everFound = 1;
        goneAt = 0;
        writeAlive();

        /* Edge 会额外建一个同名的隐藏窗口，只对"真在显示"的窗口动手 */
        int nvis = 0;
        for (int i = 0; i < n; ++i) if (IsWindowVisible(gPanelList[i])) ++nvis;

        int want = (wcscmp(mode, L"float") == 0) ? 2 : 1;
        if (want != applied) {
            applied = want;
            for (int i = 0; i < 8; ++i) seenMode[i] = 0;
            logline(L"[dock] mode=%s", want == 1 ? L"dock" : L"float");
        }

        /* Chromium 冷启动时会把 --app 窗口先建成隐藏的，不替它显示就永远看不见 */
        host = findHost();
        if (!host || !IsWindow(host)) {
            /* 浮窗是独立窗口，Dev-C++ 暂时找不到也不影响它显示 */
            if (want == 1) {
                logline(L"[dock] host gone, exit");
                break;
            }
        }

        if (nvis == 0) {
            if (want == 2) makeFloat(gPanelList[0], host);
            else ShowWindow(gPanelList[0], SW_SHOWNOACTIVATE);
        }

        for (int i = 0; i < n; ++i) {
            HWND p = gPanelList[i];
            if (!IsWindowVisible(p)) continue;
            /* 记住每个窗口已经应用过的模式，避免每轮都改样式 */
            int slot = -1;
            for (int k = 0; k < 8; ++k) if (seen[k] == p) { slot = k; break; }
            if (slot < 0) {
                for (int k = 0; k < 8; ++k) if (seen[k] == NULL) { slot = k; break; }
                if (slot < 0) slot = 0;
                seen[slot] = p;
                seenMode[slot] = 0;
            }
            if (seenMode[slot] != applied) {
                if (want == 1) makeDocked(p, host); else makeFloat(p, host);
                seenMode[slot] = applied;
            }
            if (want == 1) glue(p, host);
        }

        /* 服务端点"显示面板"时：把浮窗重新抬到最前 */
        long raiseNo = readRaise();
        if (raiseNo && raiseNo != raiseSeen) {
            raiseSeen = raiseNo;
            for (int i = 0; i < n; ++i) {
                HWND p = gPanelList[i];
                if (!IsWindowVisible(p)) continue;
                if (want == 2) bringToFront(p); else BringWindowToTop(p);
            }
        }

        if (wcscmp(mode, lastMode) != 0) wcscpy_s(lastMode, 32, mode);
        Sleep(200);
    }

    if (mutex) CloseHandle(mutex);
    clearAlive();
    return 0;
}
