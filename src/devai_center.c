/*
 * devai_center.c —— DevAI 小D 单文件中控台（纯 C + Win32 自绘）
 *
 * 一个 exe 干完这些事：
 *   - 首次运行把内嵌的 node.exe / 助手服务 / 拦截器 / 玻璃层解包到
 *     %LOCALAPPDATA%\DevAI（或 exe 旁的 DevAI-Data，放 portable.txt 即绿色版）
 *   - 自动扫描本机所有 Dev-C++，也可以手动挑目录
 *   - 填写 API Key / 模型 / 温度 / 端口，直接写 config.json
 *   - 一键接入（替换 gcc/g++/ConsolePauser，原件自动备份）或还原
 *   - 一键启动：Dev-C++ + 助手服务 + 面板 + 玻璃层
 *
 * 编译（x64）：
 *   gcc -O2 -s -municode -mwindows -o DevAI.exe devai_center.c center_res.o \
 *       -lcomctl32 -lwinhttp -lws2_32 -lshell32 -lole32 -lgdi32 -luser32 \
 *       -ladvapi32 -lmsimg32 -lcomdlg32 -ldwmapi -lversion -lshlwapi
 */

#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <commdlg.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <winhttp.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#include <math.h>

#define APP_VER    L"1.1.1"
#define APP_BUILD  L"1.1.1-1"

#define IDI_APP     101
#define IDR_PAYLOAD 200

/* ------------------------------------------------------------------ 颜色 */
/* 浅色扁平纸面：白底 + 1px 细边 + 青/淡红两色强调，全部直角 */
#define C_BG       RGB(245, 246, 247)
#define C_SIDE     RGB(236, 238, 240)
#define C_CARD     RGB(255, 255, 255)
#define C_CARD_HI  RGB(240, 243, 245)
#define C_BORDER   RGB(212, 216, 220)
#define C_TEXT     RGB(26, 31, 35)
#define C_SUB      RGB(92, 99, 105)
#define C_DIM      RGB(134, 140, 146)
#define C_ACCENT   RGB(14, 154, 167)
#define C_ACCENT_D RGB(10, 122, 133)
#define C_ACCENT2  RGB(224, 108, 99)
#define C_TINT     RGB(226, 241, 243)
#define C_GREEN    RGB(24, 138, 95)
#define C_RED      RGB(198, 62, 58)
#define C_AMBER    RGB(176, 122, 16)
#define C_INPUT    RGB(255, 255, 255)
#define C_WHITE    RGB(255, 255, 255)

/* ------------------------------------------------------------------ 全局 */
enum { P_OVERVIEW = 0, P_MODEL, P_DEVCPP, P_ADV, P_ABOUT, P_COUNT };

enum { ED_BASE = 0, ED_KEY, ED_MODEL, ED_TEMP, ED_TOK, ED_PORT, ED_COUNT };

enum {
    ID_NAV = 100,
    ID_BTN_START = 200, ID_BTN_TAB, ID_BTN_SAVE, ID_BTN_TEST, ID_BTN_SHOWKEY,
    ID_BTN_SCAN, ID_BTN_BROWSE, ID_BTN_INSTALL, ID_BTN_REMOVE,
    ID_BTN_OPENDIR, ID_BTN_OPENCFG, ID_BTN_OPENLOG, ID_BTN_SAVE_PORT,
    ID_BTN_RESTART, ID_BTN_RESET, ID_BTN_GITHUB, ID_BTN_ISSUE, ID_BTN_OPENDEV,
    ID_DEV_ROW = 300,
    ID_TG = 400
};

typedef struct {
    wchar_t apiBase[512];
    wchar_t apiKey[512];
    wchar_t model[128];
    double temperature;
    int maxTokens;
    int port;
    int autoOpenPanel;
    int explainRuntime;
    int explainWarnings;
} Config;

typedef struct {
    wchar_t root[MAX_PATH];
    wchar_t exe[MAX_PATH];
    wchar_t ver[64];
    int integrated;
} DevCpp;

typedef struct { RECT r; int id; } Hot;

static HINSTANCE g_inst;
static HWND g_hwnd;
static int g_dpi = 96;
static int g_page = P_OVERVIEW;
static int g_hover = -1;
static int g_pressed = -1;
static Config g_cfg;
static wchar_t g_home[MAX_PATH];
static wchar_t g_exePath[MAX_PATH];
static int g_elevated;

static DevCpp g_dev[24];
static int g_devCount, g_devSel;
static int g_scanDone;

static int g_serverUp;
static int g_extracting = 0, g_extractFailed = 0;
static wchar_t g_extractMsg[256];
static int g_busy = 0;               /* 正在接入 / 还原 / 启动 */
static wchar_t g_busyText[128];
static int g_cliAuto = 0;            /* --install / --uninstall 提权子进程 */
static HANDLE g_busyProc = NULL;
static int g_spin = 0;

static wchar_t g_toast[256];
static ULONGLONG g_toastUntil = 0;

static int g_testing = 0;
static int g_testOk = -1;
static wchar_t g_testMsg[512];

static Hot g_hot[128];
static int g_hotCount;
static RECT g_editRc[ED_COUNT];
static HWND g_edit[ED_COUNT];
static int g_editPage[ED_COUNT];
static RECT g_editPlaced[ED_COUNT];

static HFONT fH1, fH2, fTitle, fBody, fSmall, fNav, fMono, fNum;
static HBRUSH brBg, brSide, brCard, brInput;
static HICON icoApp, icoBig, icoSmall;

static HFONT g_fontUi;
static const int kEditId[ED_COUNT] = { 1001, 1002, 1003, 1004, 1005, 1006 };

#define SC(v) MulDiv((v), g_dpi, 96)
#define ARRAYSZ(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* ------------------------------------------------------------------ 小工具 */
static RECT LR(int x, int y, int w, int h)
{
    RECT r;
    r.left = SC(x); r.top = SC(y);
    r.right = r.left + SC(w); r.bottom = r.top + SC(h);
    return r;
}

static int file_exists(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int dir_exists(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void ensure_dir(const wchar_t *p)
{
    CreateDirectoryW(p, NULL);
}

static void toast(const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(g_toast, ARRAYSZ(g_toast) - 1, fmt, ap);
    g_toast[ARRAYSZ(g_toast) - 1] = 0;
    va_end(ap);
    g_toastUntil = GetTickCount64() + 3200;
    InvalidateRect(g_hwnd, NULL, FALSE);
}

/* UTF-8 读写 ------------------------------------------------------- */
static int read_file_utf8(const wchar_t *path, char **out, DWORD *outLen)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    DWORD size, got = 0;
    char *buf;
    if (h == INVALID_HANDLE_VALUE) return 0;
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE) { CloseHandle(h); return 0; }
    buf = (char *)malloc(size + 1);
    if (!buf) { CloseHandle(h); return 0; }
    ReadFile(h, buf, size, &got, NULL);
    buf[got] = 0;
    CloseHandle(h);
    *out = buf;
    if (outLen) *outLen = got;
    return 1;
}

static int write_file_utf8(const wchar_t *path, const wchar_t *text)
{
    int need = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    char *u8;
    HANDLE h;
    DWORD wrote = 0;
    if (need <= 0) return 0;
    u8 = (char *)malloc(need);
    if (!u8) return 0;
    WideCharToMultiByte(CP_UTF8, 0, text, -1, u8, need, NULL, NULL);
    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { free(u8); return 0; }
    WriteFile(h, u8, (DWORD)(need - 1), &wrote, NULL);
    CloseHandle(h);
    free(u8);
    return 1;
}

static void utf8_to_w(const char *s, wchar_t *out, int cap)
{
    if (!s) { out[0] = 0; return; }
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap);
    out[cap - 1] = 0;
}

/* config.json ------------------------------------------------------ */
static void config_defaults(void)
{
    wcscpy(g_cfg.apiBase, L"https://api.deepseek.com");
    g_cfg.apiKey[0] = 0;
    wcscpy(g_cfg.model, L"deepseek-chat");
    g_cfg.temperature = 0.2;
    g_cfg.maxTokens = 700;
    g_cfg.port = 47901;
    g_cfg.autoOpenPanel = 1;
    g_cfg.explainRuntime = 1;
    g_cfg.explainWarnings = 0;
}

static const wchar_t *json_find(const wchar_t *js, const wchar_t *key)
{
    wchar_t pat[160];
    const wchar_t *p;
    swprintf(pat, ARRAYSZ(pat), L"\"%ls\"", key);
    p = wcsstr(js, pat);
    if (!p) return NULL;
    p += wcslen(pat);
    while (*p && *p != L':' && *p != L'}') p++;
    if (*p != L':') return NULL;
    p++;
    while (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n') p++;
    return p;
}

static int json_str(const wchar_t *js, const wchar_t *key, wchar_t *out, int cap)
{
    const wchar_t *p = json_find(js, key);
    int n = 0;
    if (!p || *p != L'"') return 0;
    p++;
    while (*p && *p != L'"' && n < cap - 1) {
        wchar_t c = *p++;
        if (c == L'\\' && *p) {
            wchar_t e = *p++;
            switch (e) {
            case L'n': c = L'\n'; break;
            case L't': c = L'\t'; break;
            case L'r': c = L'\r'; break;
            case L'b': c = L'\b'; break;
            case L'f': c = L'\f'; break;
            case L'u': {
                wchar_t hex[5] = { 0 };
                int i;
                for (i = 0; i < 4 && p[i]; i++) hex[i] = p[i];
                p += i;
                c = (wchar_t)wcstoul(hex, NULL, 16);
                break;
            }
            default: c = e; break;
            }
        }
        out[n++] = c;
    }
    out[n] = 0;
    return 1;
}

static int json_num(const wchar_t *js, const wchar_t *key, double *out)
{
    const wchar_t *p = json_find(js, key);
    wchar_t *end = NULL;
    double v;
    if (!p) return 0;
    v = wcstod(p, &end);
    if (end == p) return 0;
    *out = v;
    return 1;
}

static int json_bool(const wchar_t *js, const wchar_t *key, int *out)
{
    const wchar_t *p = json_find(js, key);
    if (!p) return 0;
    if (wcsncmp(p, L"true", 4) == 0) { *out = 1; return 1; }
    if (wcsncmp(p, L"false", 5) == 0) { *out = 0; return 1; }
    return 0;
}

static void config_load(void)
{
    wchar_t path[MAX_PATH], *wide;
    char *raw = NULL;
    double d;
    config_defaults();
    swprintf(path, ARRAYSZ(path), L"%ls\\config.json", g_home);
    if (!read_file_utf8(path, &raw, NULL)) return;
    wide = (wchar_t *)malloc((strlen(raw) + 2) * sizeof(wchar_t));
    if (!wide) { free(raw); return; }
    utf8_to_w(raw, wide, (int)strlen(raw) + 1);
    json_str(wide, L"apiBase", g_cfg.apiBase, ARRAYSZ(g_cfg.apiBase));
    json_str(wide, L"apiKey", g_cfg.apiKey, ARRAYSZ(g_cfg.apiKey));
    json_str(wide, L"model", g_cfg.model, ARRAYSZ(g_cfg.model));
    if (json_num(wide, L"temperature", &d)) g_cfg.temperature = d;
    if (json_num(wide, L"maxTokens", &d)) g_cfg.maxTokens = (int)d;
    if (json_num(wide, L"port", &d)) g_cfg.port = (int)d;
    json_bool(wide, L"autoOpenPanel", &g_cfg.autoOpenPanel);
    json_bool(wide, L"explainRuntime", &g_cfg.explainRuntime);
    json_bool(wide, L"explainWarnings", &g_cfg.explainWarnings);
    free(wide);
    free(raw);
}

static void json_escape(const wchar_t *s, wchar_t *out, int cap)
{
    int n = 0;
    while (*s && n < cap - 2) {
        wchar_t c = *s++;
        if (c == L'"' || c == L'\\') { out[n++] = L'\\'; out[n++] = c; }
        else if (c == L'\n') { out[n++] = L'\\'; out[n++] = L'n'; }
        else if (c == L'\r') { out[n++] = L'\\'; out[n++] = L'r'; }
        else if (c < 32) { out[n++] = L' '; }
        else out[n++] = c;
    }
    out[n] = 0;
}

static void config_save(void)
{
    wchar_t path[MAX_PATH], text[8192];
    wchar_t base[1100], key[1100], model[300];
    wchar_t tmp[64];
    int port = g_cfg.port > 0 && g_cfg.port < 65536 ? g_cfg.port : 47901;
    json_escape(g_cfg.apiBase, base, ARRAYSZ(base));
    json_escape(g_cfg.apiKey, key, ARRAYSZ(key));
    json_escape(g_cfg.model, model, ARRAYSZ(model));
    swprintf(tmp, ARRAYSZ(tmp), L"%.2f", g_cfg.temperature);
    swprintf(text, ARRAYSZ(text),
        L"{\n"
        L"  \"apiBase\": \"%ls\",\n"
        L"  \"apiKey\": \"%ls\",\n"
        L"  \"model\": \"%ls\",\n"
        L"  \"temperature\": %ls,\n"
        L"  \"maxTokens\": %d,\n"
        L"  \"port\": %d,\n"
        L"  \"autoOpenPanel\": %ls,\n"
        L"  \"explainRuntime\": %ls,\n"
        L"  \"explainWarnings\": %ls\n"
        L"}\n",
        base, key, model, tmp, g_cfg.maxTokens, port,
        g_cfg.autoOpenPanel ? L"true" : L"false",
        g_cfg.explainRuntime ? L"true" : L"false",
        g_cfg.explainWarnings ? L"true" : L"false");
    swprintf(path, ARRAYSZ(path), L"%ls\\config.json", g_home);
    write_file_utf8(path, text);
}

/* 把界面上的输入框读回配置 */
static void read_edits(void)
{
    wchar_t buf[512];
    GetWindowTextW(g_edit[ED_BASE], buf, ARRAYSZ(buf));
    if (buf[0]) wcsncpy(g_cfg.apiBase, buf, ARRAYSZ(g_cfg.apiBase) - 1);
    GetWindowTextW(g_edit[ED_KEY], buf, ARRAYSZ(buf));
    wcsncpy(g_cfg.apiKey, buf, ARRAYSZ(g_cfg.apiKey) - 1);
    GetWindowTextW(g_edit[ED_MODEL], buf, ARRAYSZ(buf));
    if (buf[0]) wcsncpy(g_cfg.model, buf, ARRAYSZ(g_cfg.model) - 1);
    GetWindowTextW(g_edit[ED_TEMP], buf, ARRAYSZ(buf));
    if (buf[0]) {
        double v = wcstod(buf, NULL);
        if (v < 0) v = 0;
        if (v > 2) v = 2;
        g_cfg.temperature = v;
    }
    GetWindowTextW(g_edit[ED_TOK], buf, ARRAYSZ(buf));
    if (buf[0]) {
        int v = _wtoi(buf);
        if (v < 64) v = 64;
        if (v > 8192) v = 8192;
        g_cfg.maxTokens = v;
    }
    GetWindowTextW(g_edit[ED_PORT], buf, ARRAYSZ(buf));
    if (buf[0]) {
        int v = _wtoi(buf);
        if (v > 1024 && v < 65536) g_cfg.port = v;
    }
}

/* ------------------------------------------------------------------ 进程 / 网络 */
static int port_open(int port)
{
    SOCKET s;
    struct sockaddr_in a;
    u_long nb = 1;
    fd_set wf, ef;
    struct timeval tv;
    int ok = 0;

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ioctlsocket(s, FIONBIO, &nb);
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0) {
        ok = 1;
    } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
        FD_ZERO(&wf); FD_SET(s, &wf);
        FD_ZERO(&ef); FD_SET(s, &ef);
        tv.tv_sec = 0; tv.tv_usec = 120000;
        if (select(0, NULL, &wf, &ef, &tv) > 0 && FD_ISSET(s, &wf)) {
            int err = 0, len = sizeof(err);
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
            ok = (err == 0);
        }
    }
    closesocket(s);
    return ok;
}

static BOOL spawn_ex(const wchar_t *exe, const wchar_t *args, const wchar_t *cwd, int show)
{
    wchar_t cmd[4096];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    BOOL ok;

    if (args && args[0]) swprintf(cmd, ARRAYSZ(cmd), L"\"%ls\" %ls", exe, args);
    else swprintf(cmd, ARRAYSZ(cmd), L"\"%ls\"", exe);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = (WORD)(show ? SW_SHOWNORMAL : SW_HIDE);
    memset(&pi, 0, sizeof(pi));
    ok = CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                        show ? 0 : CREATE_NO_WINDOW, NULL, cwd, &si, &pi);
    if (!ok) return FALSE;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return TRUE;
}

static void kill_server(void)
{
    wchar_t path[MAX_PATH];
    FILE *f;
    unsigned pid = 0;
    swprintf(path, ARRAYSZ(path), L"%ls\\agent.pid", g_home);
    f = _wfopen(path, L"rb");
    if (!f) return;
    if (fscanf(f, "%u", &pid) != 1) pid = 0;
    fclose(f);
    if (pid) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (h) { TerminateProcess(h, 0); CloseHandle(h); }
    }
    DeleteFileW(path);
}

/* 简易 HTTP 调用（只连本机助手） */
static int http_call(const wchar_t *verb, const wchar_t *path, const char *body,
                     char **out, int timeoutMs)
{
    HINTERNET s, c, r;
    int status = -1;
    BOOL ok;
    DWORD size = 0;

    if (out) *out = NULL;
    s = WinHttpOpen(L"DevAI/1.1", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
    if (!s) return -1;
    WinHttpSetTimeouts(s, 1500, 1500, timeoutMs, timeoutMs);
    c = WinHttpConnect(s, L"127.0.0.1", (INTERNET_PORT)g_cfg.port, 0);
    if (!c) { WinHttpCloseHandle(s); return -1; }
    r = WinHttpOpenRequest(c, verb, path, NULL, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!r) { WinHttpCloseHandle(c); WinHttpCloseHandle(s); return -1; }
    ok = WinHttpSendRequest(r, L"Content-Type: application/json\r\n", (DWORD)-1L,
                            (LPVOID)body, body ? (DWORD)strlen(body) : 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(r, NULL);
    if (ok) {
        DWORD code = 0, len = sizeof(code);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
        status = (int)code;
    }
    if (status > 0 && out) {
        DWORD total = 0, got = 0;
        char *buf = NULL;
        do {
            char chunk[8192];
            if (!WinHttpReadData(r, chunk, sizeof(chunk), &got)) break;
            if (!got) break;
            buf = (char *)realloc(buf, total + got + 1);
            if (!buf) break;
            memcpy(buf + total, chunk, got);
            total += got;
            buf[total] = 0;
        } while (got > 0);
        *out = buf;
    }
    (void)size;
    WinHttpCloseHandle(r);
    WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return status;
}

/* ------------------------------------------------------------------ Dev-C++ 扫描 */
static void add_dev(const wchar_t *root)
{
    wchar_t exe[MAX_PATH], marker[MAX_PATH];
    wchar_t ver[64] = L"版本未知";
    int i;
    DWORD dummy, sz;
    DWORD *buf;
    VS_FIXEDFILEINFO *vi = NULL;
    UINT vlen = 0;

    if (!root || !root[0]) return;
    swprintf(exe, ARRAYSZ(exe), L"%ls\\DevCpp.exe", root);
    if (!file_exists(exe)) return;
    for (i = 0; i < g_devCount; i++)
        if (_wcsicmp(g_dev[i].root, root) == 0) return;
    if (g_devCount >= ARRAYSZ(g_dev)) return;

    sz = GetFileVersionInfoSizeW(exe, &dummy);
    if (sz) {
        buf = (DWORD *)malloc(sz);
        if (buf) {
            if (GetFileVersionInfoW(exe, 0, sz, buf) &&
                VerQueryValueW(buf, L"\\", (LPVOID *)&vi, &vlen) && vi) {
                if (HIWORD(vi->dwFileVersionMS) && LOWORD(vi->dwFileVersionMS) != 0)
                    swprintf(ver, ARRAYSZ(ver), L"%u.%u", HIWORD(vi->dwFileVersionMS),
                             LOWORD(vi->dwFileVersionMS));
                else if (HIWORD(vi->dwFileVersionMS))
                    swprintf(ver, ARRAYSZ(ver), L"%u", HIWORD(vi->dwFileVersionMS));
            }
            free(buf);
        }
    }

    wcsncpy(g_dev[g_devCount].root, root, MAX_PATH - 1);
    wcsncpy(g_dev[g_devCount].exe, exe, MAX_PATH - 1);
    wcsncpy(g_dev[g_devCount].ver, ver, 63);
    g_dev[g_devCount].integrated = 0;

    swprintf(marker, ARRAYSZ(marker), L"%ls\\devai-home.txt", root);
    if (file_exists(marker)) {
        char *raw = NULL;
        if (read_file_utf8(marker, &raw, NULL)) {
            wchar_t w[MAX_PATH];
            utf8_to_w(raw, w, MAX_PATH);
            if (_wcsicmp(w, g_home) == 0) g_dev[g_devCount].integrated = 1;
            free(raw);
        }
        if (!g_dev[g_devCount].integrated) {
            char *raw2 = NULL;
            if (read_file_utf8(marker, &raw2, NULL)) {
                wchar_t w2[MAX_PATH];
                MultiByteToWideChar(CP_ACP, 0, raw2, -1, w2, MAX_PATH);
                if (_wcsicmp(w2, g_home) == 0) g_dev[g_devCount].integrated = 1;
                free(raw2);
            }
        }
    }
    if (!g_dev[g_devCount].integrated) {
        wchar_t bin[MAX_PATH], rl[MAX_PATH];
        const wchar_t *subs[] = { L"MinGW64\\bin", L"mingw64\\bin", L"MinGW64-13.2\\bin", L"mingw32\\bin" };
        int k;
        for (k = 0; k < ARRAYSZ(subs); k++) {
            swprintf(bin, ARRAYSZ(bin), L"%ls\\%ls", root, subs[k]);
            swprintf(rl, ARRAYSZ(rl), L"%ls\\gcc-real.exe", bin);
            if (file_exists(rl)) g_dev[g_devCount].integrated = 1;
        }
    }
    g_devCount++;
}

static void scan_devcpp(void)
{
    wchar_t cand[64][MAX_PATH];
    int n = 0, i;
    wchar_t pf[MAX_PATH], pf86[MAX_PATH], lad[MAX_PATH];
    const wchar_t *drives[] = { L"C:", L"D:", L"E:", L"F:" };
    int d;

    g_devCount = 0;
    g_devSel = 0;

    /* 1) 安装记录里的路径 */
    {
        wchar_t stf[MAX_PATH];
        char *raw = NULL;
        swprintf(stf, ARRAYSZ(stf), L"%ls\\install-state.json", g_home);
        if (read_file_utf8(stf, &raw, NULL)) {
            wchar_t *wide = (wchar_t *)malloc((strlen(raw) + 2) * sizeof(wchar_t));
            if (wide) {
                const wchar_t *p;
                utf8_to_w(raw, wide, (int)strlen(raw) + 1);
                p = wide;
                while (n < ARRAYSZ(cand)) {
                    const wchar_t *q = wcsstr(p, L"\"devCppRoot\"");
                    if (!q) break;
                    q = wcschr(q, L':');
                    if (!q) break;
                    q = wcschr(q, L'"');
                    if (!q) break;
                    q++;
                    {
                        const wchar_t *e = wcschr(q, L'"');
                        int len;
                        if (!e) break;
                        len = (int)(e - q);
                        if (len > 0 && len < MAX_PATH) {
                            wcsncpy(cand[n], q, len);
                            cand[n][len] = 0;
                            n++;
                        }
                        p = e + 1;
                    }
                }
                free(wide);
            }
            free(raw);
        }
    }

    /* 2) 常见位置 */
    GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH);
    GetEnvironmentVariableW(L"ProgramFiles(x86)", pf86, MAX_PATH);
    GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH);
    {
        const wchar_t *fixedp[] = {
            L"C:\\Dev-Cpp", L"D:\\Dev-Cpp", L"E:\\Dev-Cpp", L"F:\\Dev-Cpp",
            L"D:\\dev\\Dev-Cpp", L"D:\\最新的dev\\Dev-Cpp", L"D:\\最新的dev",
            L"C:\\Program Files\\Dev-Cpp", L"C:\\Program Files (x86)\\Dev-Cpp",
            L"D:\\Program Files\\Dev-Cpp"
        };
        int k;
        for (k = 0; k < ARRAYSZ(fixedp) && n < ARRAYSZ(cand); k++)
            wcsncpy(cand[n++], fixedp[k], MAX_PATH - 1);
        if (pf[0] && n < ARRAYSZ(cand)) { swprintf(cand[n], MAX_PATH, L"%ls\\Dev-Cpp", pf); n++; }
        if (pf86[0] && n < ARRAYSZ(cand)) { swprintf(cand[n], MAX_PATH, L"%ls\\Dev-Cpp", pf86); n++; }
        if (lad[0] && n < ARRAYSZ(cand)) { swprintf(cand[n], MAX_PATH, L"%ls\\Programs\\Dev-Cpp", lad); n++; }
    }

    /* 3) 各盘根目录下一层（找 Dev-Cpp 或本身是 Dev-C++ 目录） */
    for (d = 0; d < ARRAYSZ(drives) && n < ARRAYSZ(cand) - 8; d++) {
        wchar_t base[8], pat[MAX_PATH];
        WIN32_FIND_DATAW fd;
        HANDLE h;
        swprintf(base, ARRAYSZ(base), L"%ls\\", drives[d]);
        if (!dir_exists(base)) continue;
        swprintf(pat, ARRAYSZ(pat), L"%ls*", base);
        h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            wchar_t full[MAX_PATH];
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            swprintf(full, ARRAYSZ(full), L"%ls%ls", base, fd.cFileName);
            add_dev(full);
            swprintf(full, ARRAYSZ(full), L"%ls%ls\\Dev-Cpp", base, fd.cFileName);
            add_dev(full);
        } while (FindNextFileW(h, &fd) && n < ARRAYSZ(cand) - 8);
        FindClose(h);
    }

    for (i = 0; i < n; i++) add_dev(cand[i]);
    g_scanDone = 1;
}

/* ------------------------------------------------------------------ 解包内嵌组件 */
static int extract_payload(void)
{
    HRSRC hr;
    HGLOBAL hg;
    DWORD size = 0;
    void *data;
    wchar_t tmp[MAX_PATH], tmpdir[MAX_PATH], win[MAX_PATH], cmd[2048];
    HANDLE h;
    DWORD wrote = 0;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    int ok = 0;
    wchar_t probe[MAX_PATH];

    hr = FindResourceW(g_inst, MAKEINTRESOURCEW(IDR_PAYLOAD), RT_RCDATA);
    if (!hr) { wcscpy(g_extractMsg, L"安装包不完整：找不到内嵌组件"); return 0; }
    size = SizeofResource(g_inst, hr);
    hg = LoadResource(g_inst, hr);
    data = LockResource(hg);
    if (!data || !size) { wcscpy(g_extractMsg, L"安装包损坏：组件读取失败"); return 0; }

    GetTempPathW(MAX_PATH, tmpdir);
    swprintf(tmp, ARRAYSZ(tmp), L"%lsdevai_payload_%u.zip", tmpdir, GetCurrentProcessId());
    h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { wcscpy(g_extractMsg, L"无法写入临时文件"); return 0; }
    WriteFile(h, data, size, &wrote, NULL);
    CloseHandle(h);
    if (wrote != size) {
        DeleteFileW(tmp);
        wcscpy(g_extractMsg, L"组件写入不完整，可能是磁盘空间不足");
        return 0;
    }

    GetWindowsDirectoryW(win, MAX_PATH);
    swprintf(cmd, ARRAYSZ(cmd), L"\"%ls\\System32\\tar.exe\" -xf \"%ls\" -C \"%ls\"", win, tmp, g_home);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    memset(&pi, 0, sizeof(pi));
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, g_home, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 240000);
        GetExitCodeProcess(pi.hProcess, (LPDWORD)&ok);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    DeleteFileW(tmp);

    swprintf(probe, ARRAYSZ(probe), L"%ls\\bin\\devai_gcc.exe", g_home);
    if (!file_exists(probe)) { wcscpy(g_extractMsg, L"组件解压失败，请检查磁盘空间或杀毒软件拦截"); return 0; }
    swprintf(probe, ARRAYSZ(probe), L"%ls\\node.exe", g_home);
    if (!file_exists(probe)) { wcscpy(g_extractMsg, L"Node 运行时解压失败，请重新下载"); return 0; }

    {
        wchar_t stamp[MAX_PATH];
        swprintf(stamp, ARRAYSZ(stamp), L"%ls\\build.txt", g_home);
        write_file_utf8(stamp, APP_BUILD);
    }
    return 1;
}

static int payload_needs_extract(void)
{
    wchar_t p[MAX_PATH], stamp[MAX_PATH];
    char *raw = NULL;
    int need = 1;
    swprintf(p, ARRAYSZ(p), L"%ls\\bin\\devai_gcc.exe", g_home);
    swprintf(stamp, ARRAYSZ(stamp), L"%ls\\build.txt", g_home);
    if (file_exists(p)) {
        if (read_file_utf8(stamp, &raw, NULL)) {
            if (strcmp(raw, "1.1.1-1") == 0) need = 0;
            free(raw);
        }
    }
    return need;
}

static DWORD WINAPI extract_thread(LPVOID p)
{
    int ok;
    (void)p;
    ok = extract_payload();
    PostMessageW(g_hwnd, WM_APP + 1, (WPARAM)ok, 0);
    return 0;
}

/* ------------------------------------------------------------------ 接入 / 还原 */
static int is_elevated(void)
{
    HANDLE tok;
    TOKEN_ELEVATION el;
    DWORD n = 0;
    int r = 0;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        if (GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &n))
            r = el.TokenIsElevated ? 1 : 0;
        CloseHandle(tok);
    }
    return r;
}

static void open_log_redirect(const wchar_t *logPath, HANDLE *out)
{
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    *out = CreateFileW(logPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

static BOOL run_ps_script(const wchar_t *script, const wchar_t *args, HANDLE *procOut)
{
    wchar_t win[MAX_PATH], ps[MAX_PATH], cmd[4096], log[MAX_PATH];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE logh = INVALID_HANDLE_VALUE;

    GetWindowsDirectoryW(win, MAX_PATH);
    swprintf(ps, ARRAYSZ(ps), L"%ls\\System32\\WindowsPowerShell\\v1.0\\powershell.exe", win);
    if (!file_exists(ps)) {
        swprintf(ps, ARRAYSZ(ps), L"powershell.exe");
    }
    swprintf(cmd, ARRAYSZ(cmd),
             L"\"%ls\" -NoProfile -ExecutionPolicy Bypass -File \"%ls\" %ls",
             ps, script, args ? args : L"");

    swprintf(log, ARRAYSZ(log), L"%ls\\devai-install.log", g_home);
    open_log_redirect(log, &logh);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (logh != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = logh;
        si.hStdError = logh;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    } else {
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
    }
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, g_home, &si, &pi)) {
        if (logh != INVALID_HANDLE_VALUE) CloseHandle(logh);
        return FALSE;
    }
    if (logh != INVALID_HANDLE_VALUE) CloseHandle(logh);
    CloseHandle(pi.hThread);
    if (procOut) *procOut = pi.hProcess;
    else CloseHandle(pi.hProcess);
    return TRUE;
}

static void start_install(const wchar_t *root)
{
    wchar_t script[MAX_PATH], args[2048];
    swprintf(script, ARRAYSZ(script), L"%ls\\install.ps1", g_home);
    swprintf(args, ARRAYSZ(args), L"-DevAIHome \"%ls\" -DevCppRoot \"%ls\" -SkipWait", g_home, root);
    if (run_ps_script(script, args, &g_busyProc)) {
        g_busy = 1;
        wcscpy(g_busyText, L"正在接入 Dev-C++，请稍候…");
        SetTimer(g_hwnd, 3, 350, NULL);
        InvalidateRect(g_hwnd, NULL, FALSE);
    } else {
        toast(L"无法启动接入程序，请查看日志");
    }
}

static void start_uninstall(void)
{
    wchar_t script[MAX_PATH], args[1024];
    swprintf(script, ARRAYSZ(script), L"%ls\\uninstall.ps1", g_home);
    swprintf(args, ARRAYSZ(args), L"-DevAIHome \"%ls\" -NoWait", g_home);
    if (run_ps_script(script, args, &g_busyProc)) {
        g_busy = 1;
        wcscpy(g_busyText, L"正在还原 Dev-C++，请稍候…");
        SetTimer(g_hwnd, 3, 350, NULL);
        InvalidateRect(g_hwnd, NULL, FALSE);
    } else {
        toast(L"无法启动还原程序，请查看日志");
    }
}

static int devcpp_running(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe;
    int found = 0;
    if (snap == INVALID_HANDLE_VALUE) return 0;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"DevCpp.exe") == 0) { found = 1; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

static void request_elevated(const wchar_t *verbArg)
{
    SHELLEXECUTEINFOW sei;
    wchar_t args[2048];
    swprintf(args, ARRAYSZ(args), L"%ls", verbArg);
    memset(&sei, 0, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = g_exePath;
    sei.lpParameters = args;
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        if (GetLastError() == ERROR_CANCELLED) toast(L"已取消管理员授权");
        else toast(L"无法以管理员身份启动");
    }
}

/* ------------------------------------------------------------------ 启动 / 面板 */
static void start_server(void)
{
    wchar_t node[MAX_PATH], js[MAX_PATH], args[MAX_PATH + 8];
    swprintf(node, ARRAYSZ(node), L"%ls\\node.exe", g_home);
    swprintf(js, ARRAYSZ(js), L"%ls\\server\\server.js", g_home);
    if (!file_exists(node) || !file_exists(js)) { toast(L"运行组件缺失，请重启 DevAI 自动修复"); return; }
    swprintf(args, ARRAYSZ(args), L"\"%ls\"", js);
    spawn_ex(node, args, g_home, 0);
}

static void open_panel(void)
{
    wchar_t win[MAX_PATH], ps[MAX_PATH], script[MAX_PATH], args[1024];
    GetWindowsDirectoryW(win, MAX_PATH);
    swprintf(ps, ARRAYSZ(ps), L"%ls\\System32\\WindowsPowerShell\\v1.0\\powershell.exe", win);
    swprintf(script, ARRAYSZ(script), L"%ls\\bin\\open-panel.ps1", g_home);
    if (!file_exists(script)) { toast(L"面板脚本缺失，请重启 DevAI 自动修复"); return; }
    swprintf(args, ARRAYSZ(args),
             L"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"%ls\" -Port %d",
             script, g_cfg.port);
    spawn_ex(ps, args, g_home, 0);
}

static void start_dock(void)
{
    wchar_t exe[MAX_PATH];
    swprintf(exe, ARRAYSZ(exe), L"%ls\\bin\\devai_dock.exe", g_home);
    if (file_exists(exe)) spawn_ex(exe, NULL, g_home, 0);
}

static DWORD WINAPI launch_thread(LPVOID p)
{
    int i;
    (void)p;
    start_server();
    for (i = 0; i < 60; i++) {
        if (port_open(g_cfg.port)) break;
        Sleep(150);
    }
    if (port_open(g_cfg.port)) {
        char *resp = NULL;
        http_call(L"POST", L"/api/reset", "{}", &resp, 3000);
        if (resp) free(resp);
    }
    PostMessageW(g_hwnd, WM_APP + 3, 0, 0);
    return 0;
}

static void do_start_all(void)
{
    if (g_devCount <= 0) {
        toast(L"没有找到 Dev-C++，请先在「Dev-C++」页选择目录");
        g_page = P_DEVCPP;
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    if (!g_cfg.apiKey[0]) {
        toast(L"先填写 API Key，再开始使用");
        g_page = P_MODEL;
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    if (!g_dev[g_devSel].integrated) {
        toast(L"这个 Dev-C++ 还没接入，请先点「一键接入」");
        g_page = P_DEVCPP;
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    spawn_ex(g_dev[g_devSel].exe, NULL, g_dev[g_devSel].root, 1);
    open_panel();
    start_dock();
    toast(L"正在启动 Dev-C++ 和小D…");
    CloseHandle(CreateThread(NULL, 0, launch_thread, NULL, 0, NULL));
}

static DWORD WINAPI test_thread(LPVOID p)
{
    int i;
    char *resp = NULL;
    int status;
    (void)p;
    g_testOk = -1;

    if (!port_open(g_cfg.port)) {
        start_server();
        for (i = 0; i < 50; i++) {
            if (port_open(g_cfg.port)) break;
            Sleep(200);
        }
    }
    status = http_call(L"POST", L"/api/test", "{}", &resp, 35000);
    if (status == 200 && resp) {
        wchar_t w[1024];
        utf8_to_w(resp, w, ARRAYSZ(w));
        if (wcsstr(w, L"\"ok\":true") || wcsstr(w, L"\"ok\": true")) {
            g_testOk = 1;
            swprintf(g_testMsg, ARRAYSZ(g_testMsg), L"连接成功 · %ls", g_cfg.model);
        } else {
            wchar_t *e = wcsstr(w, L"\"error\"");
            g_testOk = 0;
            if (e) {
                wchar_t *q = wcschr(e, L'"');
                wchar_t *q2 = q ? wcschr(q + 1, L'"') : NULL;
                wchar_t *q3 = NULL, *q4 = NULL;
                if (q2) { q3 = wcschr(q2 + 1, L'"'); q4 = q3 ? wcschr(q3 + 1, L'"') : NULL; }
                if (q3 && q4) { *q4 = 0; swprintf(g_testMsg, ARRAYSZ(g_testMsg), L"%ls", q3 + 1); }
                else swprintf(g_testMsg, ARRAYSZ(g_testMsg), L"接口返回异常");
            } else {
                swprintf(g_testMsg, ARRAYSZ(g_testMsg), L"接口返回异常");
            }
        }
    } else if (resp) {
        wchar_t w[512];
        utf8_to_w(resp, w, ARRAYSZ(w));
        g_testOk = 0;
        swprintf(g_testMsg, ARRAYSZ(g_testMsg), L"HTTP %d：%ls", status, w);
    } else {
        g_testOk = 0;
        wcscpy(g_testMsg, L"连不上助手服务，请点「重启助手服务」后再试");
    }
    if (resp) free(resp);
    PostMessageW(g_hwnd, WM_APP + 2, 0, 0);
    return 0;
}

/* ------------------------------------------------------------------ 绘图基元 */
static void fill_rect_c(HDC dc, RECT r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void fill_round(HDC dc, RECT r, int rad, COLORREF c)
{
    /* 直角风格：忽略半径，统一填矩形 */
    (void)rad;
    fill_rect_c(dc, r, c);
}

static void stroke_round(HDC dc, RECT r, int rad, COLORREF c, int w)
{
    HPEN p = CreatePen(PS_SOLID, w, c);
    HGDIOBJ op = SelectObject(dc, p);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    (void)rad;
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(p);
}

static void text_c(HDC dc, const wchar_t *s, RECT r, HFONT f, COLORREF c, UINT fmt)
{
    HGDIOBJ of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &r, fmt);
    SelectObject(dc, of);
}

static void text_sz(HDC dc, const wchar_t *s, int x, int y, HFONT f, COLORREF c)
{
    RECT r;
    SetRect(&r, x, y, x + SC(700), y + SC(40));
    text_c(dc, s, r, f, c, DT_LEFT | DT_TOP | DT_SINGLELINE);
}

static int text_w(HDC dc, const wchar_t *s, HFONT f)
{
    SIZE sz;
    HGDIOBJ of = SelectObject(dc, f);
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    SelectObject(dc, of);
    return sz.cx;
}

static void hot(int id, RECT r)
{
    if (g_hotCount < ARRAYSZ(g_hot)) {
        g_hot[g_hotCount].id = id;
        g_hot[g_hotCount].r = r;
        g_hotCount++;
    }
}

static void dot(HDC dc, int x, int y, int d, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, x, y, x + d, y + d);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
}

static void button(HDC dc, int draw, int id, RECT r, const wchar_t *label, int kind, int enabled)
{
    int hov = (g_hover == id);
    COLORREF bg, fg, bd;
    RECT rr = r;
    int rad = 0;

    if (kind == 0 || kind == 3) {   /* primary */
        bg = hov ? C_ACCENT_D : C_ACCENT;
        fg = C_WHITE;
        bd = bg;
    } else if (kind == 1) {         /* secondary */
        bg = hov ? C_CARD_HI : C_CARD;
        fg = C_TEXT;
        bd = hov ? C_ACCENT : C_BORDER;
    } else {                        /* ghost */
        bg = hov ? C_TINT : C_CARD;
        fg = hov ? C_ACCENT_D : C_SUB;
        bd = C_BORDER;
    }
    if (!enabled) {
        bg = RGB(238, 240, 241);
        fg = RGB(166, 171, 176);
        bd = RGB(219, 222, 225);
    }
    if (draw) {
        fill_round(dc, rr, rad, bg);
        stroke_round(dc, rr, rad, bd, 1);
        text_c(dc, label, rr, fBody, fg, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    hot(id, r);
}

static void toggle(HDC dc, int draw, int id, RECT r, int on)
{
    /* 方形开关：轨道 + 方形滑块 */
    int pad = SC(3);
    int kd = (r.bottom - r.top) - pad * 2;
    RECT knob, track = r;
    if (draw) {
        fill_round(dc, track, 0, on ? C_ACCENT : RGB(206, 211, 215));
        if (on) {
            knob.left = r.right - pad - kd;
            knob.top = r.top + pad;
            knob.right = knob.left + kd;
            knob.bottom = r.bottom - pad;
        } else {
            knob.left = r.left + pad;
            knob.top = r.top + pad;
            knob.right = knob.left + kd;
            knob.bottom = r.bottom - pad;
        }
        fill_round(dc, knob, 0, C_WHITE);
        stroke_round(dc, knob, 0, on ? C_WHITE : RGB(178, 184, 189), 1);
    }
    hot(id, r);
}

static void pill(HDC dc, RECT r, const wchar_t *label, COLORREF dotc, COLORREF txtc)
{
    fill_round(dc, r, 0, C_CARD);
    stroke_round(dc, r, 0, C_BORDER, 1);
    if (dotc) dot(dc, r.left + SC(10), r.top + ((r.bottom - r.top) - SC(6)) / 2, SC(6), dotc);
    {
        RECT tr = r;
        tr.left += dotc ? SC(22) : SC(10);
        tr.right -= SC(10);
        text_c(dc, label, tr, fSmall, txtc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
}

/* ------------------------------------------------------------------ 图标 */
static void nav_icon(HDC dc, int kind, int x, int y, COLORREF c)
{
    HPEN p = CreatePen(PS_SOLID, SC(2) > 1 ? SC(2) : 1, c);
    HGDIOBJ op = SelectObject(dc, p);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    int s = SC(16);
    switch (kind) {
    case 0:
        Rectangle(dc, x, y, x + SC(7), y + SC(7));
        Rectangle(dc, x + SC(9), y, x + SC(16), y + SC(7));
        Rectangle(dc, x, y + SC(9), x + SC(7), y + SC(16));
        Rectangle(dc, x + SC(9), y + SC(9), x + SC(16), y + SC(16));
        break;
    case 1:
        MoveToEx(dc, x + s / 2, y, NULL);
        LineTo(dc, x + s / 2 + SC(3), y + s / 2 - SC(3));
        LineTo(dc, x + s, y + s / 2);
        LineTo(dc, x + s / 2 + SC(3), y + s / 2 + SC(3));
        LineTo(dc, x + s / 2, y + s);
        LineTo(dc, x + s / 2 - SC(3), y + s / 2 + SC(3));
        LineTo(dc, x, y + s / 2);
        LineTo(dc, x + s / 2 - SC(3), y + s / 2 - SC(3));
        LineTo(dc, x + s / 2, y);
        break;
    case 2:
        Rectangle(dc, x, y + SC(2), x + s, y + s - SC(1));
        MoveToEx(dc, x, y + SC(6), NULL);
        LineTo(dc, x + s, y + SC(6));
        break;
    case 3:
        MoveToEx(dc, x, y + SC(4), NULL);
        LineTo(dc, x + s, y + SC(4));
        MoveToEx(dc, x, y + SC(9), NULL);
        LineTo(dc, x + s, y + SC(9));
        MoveToEx(dc, x, y + SC(14), NULL);
        LineTo(dc, x + s, y + SC(14));
        dot(dc, x + SC(5), y + SC(2), SC(5), c);
        dot(dc, x + SC(11), y + SC(7), SC(5), c);
        dot(dc, x + SC(7), y + SC(12), SC(5), c);
        break;
    default:
        Ellipse(dc, x, y, x + s, y + s);
        dot(dc, x + s / 2 - SC(1), y + SC(4), SC(2), c);
        MoveToEx(dc, x + s / 2, y + SC(8), NULL);
        LineTo(dc, x + s / 2, y + SC(12));
        break;
    }
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(p);
}

/* ------------------------------------------------------------------ 布局 + 绘制 */
static void draw_sidebar(HDC dc, int draw)
{
    RECT side = LR(0, 0, 208, 640);
    RECT edge = LR(207, 0, 1, 640);
    if (draw) {
        fill_rect_c(dc, side, C_SIDE);
        fill_rect_c(dc, edge, C_BORDER);
    }

    if (draw && icoApp) {
        DrawIconEx(dc, SC(24), SC(24), icoApp, SC(40), SC(40), 0, NULL, DI_NORMAL);
    }
    text_sz(dc, L"小D · DevAI", SC(76), SC(26), fTitle, C_TEXT);
    text_sz(dc, L"Dev-C++ 智能助手", SC(76), SC(49), fSmall, C_DIM);

    {
        const wchar_t *names[P_COUNT] = { L"概览", L"模型设置", L"Dev-C++", L"高级", L"关于" };
        int i;
        for (i = 0; i < P_COUNT; i++) {
            RECT r = LR(16, 112 + i * 46, 176, 40);
            int act = (g_page == i);
            int id = ID_NAV + i;
            if (draw) {
                if (act) {
                    fill_round(dc, r, 0, C_CARD);
                    stroke_round(dc, r, 0, C_BORDER, 1);
                } else if (g_hover == id) {
                    fill_round(dc, r, 0, RGB(230, 233, 235));
                }
                if (act) {
                    RECT bar = LR(16, 112 + i * 46, 3, 40);
                    fill_round(dc, bar, 0, C_ACCENT);
                }
                nav_icon(dc, i, SC(34), SC(112 + i * 46 + 12), act ? C_ACCENT : C_SUB);
                text_sz(dc, names[i], SC(64), SC(112 + i * 46 + 12), fNav, act ? C_TEXT : C_SUB);
            }
            hot(id, r);
        }
    }

    if (draw) {
        /* 底部状态 */
        COLORREF dc2 = g_serverUp ? C_GREEN : RGB(178, 184, 189);
        dot(dc, SC(26), SC(562), SC(7), dc2);
        text_sz(dc, g_serverUp ? L"助手服务运行中" : L"助手服务未启动", SC(40), SC(556), fSmall,
                g_serverUp ? C_SUB : C_DIM);
        text_sz(dc, L"v" APP_VER L" · MIT License", SC(26), SC(584), fSmall, RGB(158, 164, 170));
    }
}

static void draw_header(HDC dc, int draw, const wchar_t *title, const wchar_t *sub, int showPill)
{
    if (draw) {
        text_sz(dc, title, SC(236), SC(28), fH1, C_TEXT);
        text_sz(dc, sub, SC(236), SC(62), fBody, C_SUB);
        if (showPill) {
            RECT pr = LR(760, 34, 152, 28);
            pill(dc, pr, g_serverUp ? L"服务运行中" : L"服务未启动",
                 g_serverUp ? C_GREEN : RGB(178, 184, 189),
                 g_serverUp ? C_GREEN : C_SUB);
        }
    }
}

static void draw_overview(HDC dc, int draw)
{
    RECT hero = LR(236, 96, 676, 156);
    RECT btn;
    if (draw) {
        fill_rect_c(dc, hero, C_CARD);
        stroke_round(dc, hero, 0, C_BORDER, 1);
        /* 左侧青 / 淡红双色竖条，呼应图标 */
        {
            RECT b1, b2;
            b1.left = hero.left; b1.right = hero.left + SC(5);
            b1.top = hero.top; b1.bottom = (hero.top + hero.bottom) / 2;
            b2 = b1; b2.top = b1.bottom; b2.bottom = hero.bottom;
            fill_rect_c(dc, b1, C_ACCENT);
            fill_rect_c(dc, b2, C_ACCENT2);
        }
        text_sz(dc, L"让 Dev-C++ 会说人话", SC(268), SC(124), fH2, C_TEXT);
        text_sz(dc, L"编译报错时，小D 直接在出错那一行讲清「为什么错 / 改哪里」", SC(268), SC(158), fBody, C_SUB);
        text_sz(dc, L"不用切窗口，不用翻日志。", SC(268), SC(183), fBody, C_DIM);
        if (icoBig) DrawIconEx(dc, SC(772), SC(118), icoBig, SC(96), SC(96), 0, NULL, DI_NORMAL);
    }
    btn = LR(268, 200, 132, 38);
    button(dc, draw, ID_BTN_START, btn, L"开始使用", 0, !g_extracting);

    {
        const wchar_t *lab[3], *val[3], *hint[3];
        int i;
        COLORREF dcs[3];
        wchar_t devtxt[64];
        if (g_devCount <= 0) swprintf(devtxt, ARRAYSZ(devtxt), L"未找到");
        else {
            int cnt = 0;
            for (i = 0; i < g_devCount; i++) if (g_dev[i].integrated) cnt++;
            if (cnt) swprintf(devtxt, ARRAYSZ(devtxt), L"%d 个版本已接入", cnt);
            else wcscpy(devtxt, L"尚未接入");
        }
        lab[0] = L"助手服务"; lab[1] = L"Dev-C++"; lab[2] = L"AI 模型";
        val[0] = g_serverUp ? L"运行中" : L"未启动";
        val[1] = devtxt;
        val[2] = g_cfg.apiKey[0] ? g_cfg.model : L"未配置 Key";
        hint[0] = L"127.0.0.1"; hint[1] = L"6.4 / 7.1 都支持"; hint[2] = L"OpenAI 兼容接口";
        dcs[0] = g_serverUp ? C_GREEN : RGB(178, 184, 189);
        dcs[1] = (g_devCount > 0 && g_dev[0].integrated) ? C_GREEN : C_AMBER;
        dcs[2] = g_cfg.apiKey[0] ? C_GREEN : C_AMBER;

        for (i = 0; i < 3; i++) {
            RECT t = LR(236 + i * 229, 268, 217, 88);
            if (draw) {
                fill_round(dc, t, SC(14), C_CARD);
                stroke_round(dc, t, SC(14), C_BORDER, 1);
                dot(dc, t.left + SC(18), t.top + SC(20), SC(8), dcs[i]);
                text_sz(dc, lab[i], SC(236 + i * 229 + 34), SC(282), fSmall, C_SUB);
                text_sz(dc, val[i], SC(236 + i * 229 + 18), SC(300), fTitle, C_TEXT);
                text_sz(dc, hint[i], SC(236 + i * 229 + 18), SC(328), fSmall, C_DIM);
            }
        }
    }

    {
        RECT card = LR(236, 372, 676, 208);
        if (draw) {
            fill_round(dc, card, SC(14), C_CARD);
            stroke_round(dc, card, SC(14), C_BORDER, 1);
            text_sz(dc, L"三步开始", SC(264), SC(390), fTitle, C_TEXT);
        }
        {
            const wchar_t *t1[3] = { L"填写 API Key", L"接入 Dev-C++", L"打开 Dev-C++ 开始写代码" };
            const wchar_t *t2[3] = {
                L"在「模型设置」里填入 DeepSeek 或任意兼容接口的 Key",
                L"在「Dev-C++」页选中版本，点一键接入（原文件自动备份）",
                L"之后每次从本控制台启动，小D 都会跟着 Dev-C++ 一起工作"
            };
            const wchar_t *right[3];
            COLORREF rightc[3];
            int cnt = 0, i;
            for (i = 0; i < g_devCount; i++) if (g_dev[i].integrated) cnt++;
            right[0] = g_cfg.apiKey[0] ? L"已完成" : L"待填写";
            right[1] = cnt ? L"已接入" : L"待接入";
            right[2] = g_serverUp ? L"服务已就绪" : L"随时可用";
            rightc[0] = g_cfg.apiKey[0] ? C_GREEN : C_AMBER;
            rightc[1] = cnt ? C_GREEN : C_AMBER;
            rightc[2] = C_DIM;

            for (i = 0; i < 3; i++) {
                int y = 426 + i * 50;
                RECT row = LR(256, y - 8, 636, 44);
                if (draw) {
                    RECT circ = LR(264, y - 2, 26, 26);
                    fill_round(dc, circ, 0, C_TINT);
                    stroke_round(dc, circ, 0, C_ACCENT, 1);
                    {
                        wchar_t num[4];
                        swprintf(num, ARRAYSZ(num), L"%d", i + 1);
                        text_c(dc, num, circ, fSmall, C_ACCENT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    }
                    text_sz(dc, t1[i], SC(306), SC(y - 4), fBody, C_TEXT);
                    text_sz(dc, t2[i], SC(306), SC(y + 15), fSmall, C_DIM);
                    {
                        RECT rr = LR(820, y, 76, 20);
                        text_c(dc, right[i], rr, fSmall, rightc[i], DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                    }
                }
                (void)row;
            }
        }
    }
}

static void draw_model(HDC dc, int draw)
{
    RECT c1 = LR(236, 96, 676, 254);
    RECT c2 = LR(236, 366, 676, 176);
    int i;

    if (draw) {
        fill_round(dc, c1, SC(14), C_CARD);
        stroke_round(dc, c1, SC(14), C_BORDER, 1);
        text_sz(dc, L"接口配置", SC(264), SC(112), fTitle, C_TEXT);
        text_sz(dc, L"兼容 DeepSeek / OpenAI / 各类中转站，Key 只保存在本机 config.json", SC(264), SC(134), fSmall, C_DIM);

        text_sz(dc, L"API 地址", SC(264), SC(160), fSmall, C_SUB);
        text_sz(dc, L"API Key", SC(264), SC(216), fSmall, C_SUB);
        text_sz(dc, L"模型名", SC(264), SC(280), fSmall, C_SUB);
        text_sz(dc, L"温度", SC(564), SC(280), fSmall, C_SUB);
        text_sz(dc, L"最大回复长度", SC(714), SC(280), fSmall, C_SUB);

        /* 输入框底：白底 + 细边 */
        {
            RECT ir[6];
            int k;
            ir[0] = LR(264, 176, 620, 38);
            ir[1] = LR(264, 230, 536, 38);
            ir[2] = LR(800, 230, 84, 38);
            ir[3] = LR(264, 300, 292, 38);
            ir[4] = LR(564, 300, 140, 38);
            ir[5] = LR(714, 300, 170, 38);
            for (k = 0; k < 6; k++) {
                fill_round(dc, ir[k], 0, C_INPUT);
                stroke_round(dc, ir[k], 0, C_BORDER, 1);
            }
        }
        text_c(dc, L"显示", LR(800, 230, 84, 38), fSmall, C_SUB, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    hot(ID_BTN_SHOWKEY, LR(800, 230, 84, 38));

    if (draw) {
        fill_round(dc, c2, SC(14), C_CARD);
        stroke_round(dc, c2, SC(14), C_BORDER, 1);
        text_sz(dc, L"讲解偏好", SC(264), SC(382), fTitle, C_TEXT);
    }
    {
        const wchar_t *lab[3] = { L"运行时崩溃也讲一讲", L"把编译警告也解释一下", L"启动时自动打开助手面板" };
        const wchar_t *sub[3] = {
            L"程序异常退出时，解释退出码和可能原因",
            L"警告不影响运行，默认不打扰，开着更细致",
            L"关掉后只开 Dev-C++，需要时再手动打开面板"
        };
        int *vals[3];
        vals[0] = &g_cfg.explainRuntime;
        vals[1] = &g_cfg.explainWarnings;
        vals[2] = &g_cfg.autoOpenPanel;
        for (i = 0; i < 3; i++) {
            int y = 412 + i * 42;
            if (draw) {
                text_sz(dc, lab[i], SC(264), SC(y), fBody, C_TEXT);
                text_sz(dc, sub[i], SC(264), SC(y + 18), fSmall, C_DIM);
            }
            toggle(dc, draw, ID_TG + i, LR(838, y - 2, 44, 24), *vals[i]);
        }
    }

    button(dc, draw, ID_BTN_SAVE, LR(236, 556, 120, 40), L"保存设置", 0, 1);
    button(dc, draw, ID_BTN_TEST, LR(368, 556, 116, 40), g_testing ? L"测试中…" : L"测试连接", 1, !g_testing);
    if (draw && (g_testOk >= 0 || g_testing)) {
        COLORREF c = g_testing ? C_SUB : (g_testOk ? C_GREEN : C_RED);
        const wchar_t *s = g_testing ? L"正在连接模型接口…" : g_testMsg;
        text_sz(dc, s, SC(500), SC(568), fSmall, c);
    }
}

static void draw_devcpp(HDC dc, int draw)
{
    RECT c1 = LR(236, 96, 676, 320);
    RECT c2 = LR(236, 432, 676, 120);
    int i;

    if (draw) {
        fill_round(dc, c1, SC(14), C_CARD);
        stroke_round(dc, c1, SC(14), C_BORDER, 1);
        text_sz(dc, L"本机的 Dev-C++", SC(264), SC(112), fTitle, C_TEXT);
        text_sz(dc, L"选一个版本接入；装了多个也没关系，可以逐个接入", SC(264), SC(134), fSmall, C_DIM);
    }
    button(dc, draw, ID_BTN_SCAN, LR(716, 106, 84, 30), L"重新扫描", 2, 1);
    button(dc, draw, ID_BTN_BROWSE, LR(808, 106, 76, 30), L"浏览…", 2, 1);

    if (g_scanDone && g_devCount == 0) {
        if (draw) text_sz(dc, L"没有找到 Dev-C++，点「浏览…」手动选择 DevCpp.exe", SC(264), SC(200), fBody, C_AMBER);
    }
    for (i = 0; i < g_devCount && i < 4; i++) {
        RECT row = LR(252, 152 + i * 54, 644, 50);
        int sel = (i == g_devSel);
        int id = ID_DEV_ROW + i;
        if (draw) {
            if (sel) {
                fill_round(dc, row, 0, C_TINT);
                stroke_round(dc, row, 0, C_ACCENT, 1);
            } else if (g_hover == id) {
                fill_round(dc, row, 0, C_CARD_HI);
            } else {
                stroke_round(dc, row, 0, C_BORDER, 1);
            }
            {
                /* 方形选择框 */
                RECT cir = LR(268, 152 + i * 54 + 16, 18, 18);
                fill_round(dc, cir, 0, C_WHITE);
                stroke_round(dc, cir, 0, sel ? C_ACCENT : C_BORDER, 1);
                if (sel) {
                    RECT in = cir;
                    InflateRect(&in, -SC(5), -SC(5));
                    fill_round(dc, in, 0, C_ACCENT);
                }
            }
            {
                wchar_t line[96];
                swprintf(line, ARRAYSZ(line), L"Dev-C++ %ls", g_dev[i].ver);
                text_sz(dc, line, SC(300), SC(152 + i * 54 + 6), fBody, C_TEXT);
            }
            {
                RECT pr = LR(300, 152 + i * 54 + 26, 470, 16);
                text_c(dc, g_dev[i].root, pr, fSmall, C_DIM,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            if (g_dev[i].integrated) {
                RECT bg = LR(792, 152 + i * 54 + 14, 88, 22);
                fill_round(dc, bg, 0, RGB(226, 243, 236));
                stroke_round(dc, bg, 0, RGB(184, 219, 203), 1);
                text_c(dc, L"已接入", bg, fSmall, C_GREEN, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }
        hot(id, row);
    }
    if (g_devCount > 4 && draw) {
        wchar_t more[64];
        swprintf(more, ARRAYSZ(more), L"还有 %d 个版本，重新扫描后可逐个接入", g_devCount - 4);
        text_sz(dc, more, SC(264), SC(390), fSmall, C_DIM);
    }

    if (draw) {
        int cnt = 0;
        wchar_t line[128];
        fill_round(dc, c2, SC(14), C_CARD);
        stroke_round(dc, c2, SC(14), C_BORDER, 1);
        for (i = 0; i < g_devCount; i++) if (g_dev[i].integrated) cnt++;
        if (cnt) swprintf(line, ARRAYSZ(line), L"已接入 %d 个 Dev-C++ 版本", cnt);
        else wcscpy(line, L"尚未接入");
        text_sz(dc, line, SC(264), SC(450), fTitle, cnt ? C_GREEN : C_TEXT);
        text_sz(dc, L"接入会替换 gcc / g++ / ConsolePauser，原文件自动备份到 Dev-C++ 目录，可随时还原。",
                SC(264), SC(474), fSmall, C_DIM);
    }
    button(dc, draw, ID_BTN_INSTALL, LR(264, 496, 120, 38), L"一键接入", 0, !g_busy);
    button(dc, draw, ID_BTN_REMOVE, LR(396, 496, 120, 38), L"移除接入", 1, !g_busy);
    button(dc, draw, ID_BTN_OPENDEV, LR(528, 496, 124, 38), L"打开 Dev-C++", 2, 1);
}

static void draw_adv(HDC dc, int draw)
{
    RECT c1 = LR(236, 96, 676, 150);
    RECT c2 = LR(236, 262, 676, 150);
    RECT c3 = LR(236, 428, 676, 120);

    if (draw) {
        fill_round(dc, c1, SC(14), C_CARD);
        stroke_round(dc, c1, SC(14), C_BORDER, 1);
        text_sz(dc, L"助手服务", SC(264), SC(112), fTitle, C_TEXT);
        text_sz(dc, L"本地端口", SC(264), SC(140), fSmall, C_SUB);
        fill_round(dc, LR(264, 158, 120, 38), 0, C_INPUT);
        stroke_round(dc, LR(264, 158, 120, 38), 0, C_BORDER, 1);
        text_sz(dc, L"只监听 127.0.0.1；被占用时会自动顺延到下一个端口", SC(404), SC(166), fSmall, C_DIM);
    }
    button(dc, draw, ID_BTN_SAVE_PORT, LR(788, 158, 96, 38), L"保存端口", 1, 1);

    if (draw) {
        fill_round(dc, c2, SC(14), C_CARD);
        stroke_round(dc, c2, SC(14), C_BORDER, 1);
        text_sz(dc, L"数据与日志", SC(264), SC(278), fTitle, C_TEXT);
        text_sz(dc, L"运行目录", SC(264), SC(306), fSmall, C_SUB);
        {
            RECT pr = LR(336, 306, 376, 18);
            text_c(dc, g_home, pr, fMono, C_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }
    button(dc, draw, ID_BTN_OPENDIR, LR(264, 336, 104, 36), L"打开目录", 1, 1);
    button(dc, draw, ID_BTN_OPENCFG, LR(378, 336, 120, 36), L"打开配置文件", 1, 1);
    button(dc, draw, ID_BTN_OPENLOG, LR(508, 336, 104, 36), L"查看日志", 1, 1);

    if (draw) {
        fill_round(dc, c3, SC(14), C_CARD);
        stroke_round(dc, c3, SC(14), C_BORDER, 1);
        text_sz(dc, L"维护", SC(264), SC(444), fTitle, C_TEXT);
        text_sz(dc, L"改了端口或换了模型，重启助手服务即可生效", SC(264), SC(468), fSmall, C_DIM);
    }
    button(dc, draw, ID_BTN_RESTART, LR(264, 492, 130, 36), L"重启助手服务", 1, !g_busy);
    button(dc, draw, ID_BTN_RESET, LR(404, 492, 124, 36), L"清空分析记录", 2, !g_busy);
}

static void draw_about(HDC dc, int draw)
{
    if (draw) {
        if (icoBig) DrawIconEx(dc, SC(236), SC(110), icoBig, SC(96), SC(96), 0, NULL, DI_NORMAL);
        text_sz(dc, L"小D · DevAI", SC(352), SC(116), fH2, C_TEXT);
        text_sz(dc, L"v" APP_VER L"   单文件版", SC(354), SC(150), fBody, C_SUB);
        {
            RECT pr = LR(354, 174, 120, 24);
            pill(dc, pr, L"MIT License", C_GREEN, C_GREEN);
        }
        text_sz(dc, L"给 Dev-C++ 贴一层会讲人话的玻璃。", SC(236), SC(238), fTitle, C_TEXT);
        text_sz(dc, L"编译报错时，小D 在出错那一行旁划线，用三个框讲清「为什么错 / 改哪里 / 记住什么」。", SC(236), SC(266), fBody, C_SUB);
        text_sz(dc, L"API Key 只保存在本机 config.json，除了 AI 接口不会发给任何地方。", SC(236), SC(290), fBody, C_SUB);
        text_sz(dc, L"感谢小龙Dev 与 Dev-C++ 社区。", SC(236), SC(326), fSmall, C_DIM);
        text_sz(dc, L"运行组件：Node.js（内嵌，无需安装） · 拦截器与玻璃层：纯 C + Win32", SC(236), SC(350), fSmall, C_DIM);
    }
    button(dc, draw, ID_BTN_GITHUB, LR(236, 386, 132, 38), L"GitHub 主页", 1, 1);
    button(dc, draw, ID_BTN_ISSUE, LR(380, 386, 132, 38), L"反馈问题", 2, 1);
}

/* 输入框位置（逻辑坐标） */
static void layout_edits(void)
{
    int i;
    for (i = 0; i < ED_COUNT; i++) {
        g_editRc[i].left = g_editRc[i].top = g_editRc[i].right = g_editRc[i].bottom = 0;
        g_editPage[i] = -1;
    }
    g_editRc[ED_BASE] = LR(276, 182, 596, 26);   g_editPage[ED_BASE] = P_MODEL;
    g_editRc[ED_KEY] = LR(276, 236, 524, 26);    g_editPage[ED_KEY] = P_MODEL;
    g_editRc[ED_MODEL] = LR(276, 306, 268, 26);  g_editPage[ED_MODEL] = P_MODEL;
    g_editRc[ED_TEMP] = LR(576, 306, 116, 26);   g_editPage[ED_TEMP] = P_MODEL;
    g_editRc[ED_TOK] = LR(726, 306, 146, 26);    g_editPage[ED_TOK] = P_MODEL;
    g_editRc[ED_PORT] = LR(274, 166, 100, 24);   g_editPage[ED_PORT] = P_ADV;
}

static void place_edits(void)
{
    int i;
    for (i = 0; i < ED_COUNT; i++) {
        int show = (g_editPage[i] == g_page) && !g_extracting;
        RECT r = g_editRc[i];
        if (g_editPage[i] < 0) show = 0;
        if (!show) { ShowWindow(g_edit[i], SW_HIDE); continue; }
        if (!EqualRect(&r, &g_editPlaced[i])) {
            SetWindowPos(g_edit[i], NULL, r.left, r.top, r.right - r.left, r.bottom - r.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            g_editPlaced[i] = r;
        }
        ShowWindow(g_edit[i], SW_SHOW);
    }
}

static void draw_marquee(HDC dc);

static void draw_all(HDC dc)
{
    RECT client;
    GetClientRect(g_hwnd, &client);

    g_hotCount = 0;
    fill_rect_c(dc, client, C_BG);
    draw_sidebar(dc, 1);

    switch (g_page) {
    case P_OVERVIEW:
        draw_header(dc, 1, L"概览", L"一个窗口管好 Dev-C++ 和小D 的一切", 1);
        draw_overview(dc, 1);
        break;
    case P_MODEL:
        draw_header(dc, 1, L"模型设置", L"填好 Key 就能用；换接口、换模型也在这里", 0);
        draw_model(dc, 1);
        break;
    case P_DEVCPP:
        draw_header(dc, 1, L"Dev-C++", L"自动扫描本机所有版本，选一个接入", 0);
        draw_devcpp(dc, 1);
        break;
    case P_ADV:
        draw_header(dc, 1, L"高级", L"端口、数据目录和一点维护操作", 0);
        draw_adv(dc, 1);
        break;
    default:
        draw_header(dc, 1, L"关于", L"小D · DevAI", 0);
        draw_about(dc, 1);
        break;
    }

    if (g_extracting || g_busy || g_extractFailed) {
        int save = SaveDC(dc);
        RECT full;
        GetClientRect(g_hwnd, &full);
        /* 半透明不清楚，直接压一层浅灰 */
        fill_rect_c(dc, full, RGB(233, 236, 239));
        draw_marquee(dc);
        RestoreDC(dc, save);
    }
    if (GetTickCount64() < g_toastUntil) {
        int tw = text_w(dc, g_toast, fBody) + SC(36);
        RECT tr;
        int h = SC(38);
        tr.left = (client.right - tw) / 2;
        tr.right = tr.left + tw;
        tr.bottom = client.bottom - SC(22);
        tr.top = tr.bottom - h;
        fill_round(dc, tr, 0, C_CARD);
        stroke_round(dc, tr, 0, C_BORDER, 1);
        text_c(dc, g_toast, tr, fBody, C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

static void draw_marquee(HDC dc)
{
    RECT client, card;
    const wchar_t *line1, *line2;
    int cx, cy, r, ch;

    GetClientRect(g_hwnd, &client);
    ch = g_extractFailed ? SC(198) : SC(170);
    card.left = (client.right - SC(460)) / 2;
    card.top = (client.bottom - ch) / 2;
    card.right = card.left + SC(460);
    card.bottom = card.top + ch;
    fill_round(dc, card, SC(18), C_CARD);
    stroke_round(dc, card, 0, g_extractFailed ? RGB(232, 196, 194) : C_BORDER, 1);

    cx = (card.left + card.right) / 2;
    cy = card.top + SC(52);
    r = SC(17);
    if (g_extractFailed) {
        HPEN p = CreatePen(PS_SOLID, SC(3), C_RED);
        HGDIOBJ op = SelectObject(dc, p);
        int k = r - SC(6);
        Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
        MoveToEx(dc, cx - k, cy - k, NULL);
        LineTo(dc, cx + k, cy + k);
        MoveToEx(dc, cx + k, cy - k, NULL);
        LineTo(dc, cx - k, cy + k);
        SelectObject(dc, op);
        DeleteObject(p);
    } else {
        HPEN p = CreatePen(PS_SOLID, SC(3), RGB(214, 218, 222));
        HPEN p2 = CreatePen(PS_SOLID, SC(3), C_ACCENT);
        HGDIOBJ op;
        op = SelectObject(dc, p);
        Arc(dc, cx - r, cy - r, cx + r, cy + r, cx + r, cy, cx - r, cy);
        SelectObject(dc, p2);
        {
            int a1 = g_spin % 360;
            double rad1 = a1 * 3.14159265358979 / 180.0;
            double rad2 = (a1 + 90) * 3.14159265358979 / 180.0;
            Arc(dc, cx - r, cy - r, cx + r, cy + r,
                cx + (int)(r * cos(rad1)), cy - (int)(r * sin(rad1)),
                cx + (int)(r * cos(rad2)), cy - (int)(r * sin(rad2)));
        }
        SelectObject(dc, op);
        DeleteObject(p);
        DeleteObject(p2);
    }

    line1 = g_extracting ? L"首次启动，正在释放运行组件…" : g_busyText;
    if (g_extractFailed) line1 = L"组件释放失败";
    line2 = g_extracting ? L"内嵌 Node 运行时，只需要几秒钟" : L"完成后会自动刷新状态";
    if (g_extractFailed) line2 = g_extractMsg;

    {
        RECT tr;
        tr.left = card.left; tr.right = card.right;
        tr.top = card.top + SC(84); tr.bottom = tr.top + SC(28);
        text_c(dc, line1, tr, fTitle, g_extractFailed ? C_RED : C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        tr.top += SC(30); tr.bottom = tr.top + SC(24);
        text_c(dc, line2, tr, fSmall, C_DIM, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (g_extractFailed) {
            tr.top += SC(30); tr.bottom = tr.top + SC(22);
            text_c(dc, L"点击窗口任意位置重试", tr, fSmall, C_ACCENT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
}

/* ------------------------------------------------------------------ 交互 */
static int hit_test(POINT pt)
{
    int i;
    for (i = g_hotCount - 1; i >= 0; i--)
        if (PtInRect(&g_hot[i].r, pt)) return g_hot[i].id;
    return -1;
}

static void set_page(int p)
{
    if (p == g_page) return;
    g_page = p;
    SetFocus(g_hwnd);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void do_save(int quiet)
{
    read_edits();
    config_save();
    if (!quiet) toast(L"设置已保存");
    else toast(L"设置已保存，正在测试连接…");
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void open_path(const wchar_t *path)
{
    ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL);
}

static void on_click(int id)
{
    if (g_extracting) { toast(L"正在准备组件，请稍候"); return; }
    if (g_extractFailed) {
        g_extractFailed = 0;
        g_extracting = 1;
        g_extractMsg[0] = 0;
        CloseHandle(CreateThread(NULL, 0, extract_thread, NULL, 0, NULL));
        return;
    }

    if (id >= ID_NAV && id < ID_NAV + P_COUNT) { set_page(id - ID_NAV); return; }
    if (id >= ID_DEV_ROW && id < ID_DEV_ROW + 24) {
        g_devSel = id - ID_DEV_ROW;
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    if (id >= ID_TG && id < ID_TG + 3) {
        int n = id - ID_TG;
        if (n == 0) g_cfg.explainRuntime = !g_cfg.explainRuntime;
        else if (n == 1) g_cfg.explainWarnings = !g_cfg.explainWarnings;
        else g_cfg.autoOpenPanel = !g_cfg.autoOpenPanel;
        config_save();
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }

    switch (id) {
    case ID_BTN_START:
        do_start_all();
        break;
    case ID_BTN_SAVE:
        do_save(0);
        break;
    case ID_BTN_TEST:
        if (!g_cfg.apiKey[0]) { toast(L"先填写 API Key"); break; }
        do_save(1);
        g_testing = 1;
        g_testOk = -1;
        InvalidateRect(g_hwnd, NULL, FALSE);
        CloseHandle(CreateThread(NULL, 0, test_thread, NULL, 0, NULL));
        break;
    case ID_BTN_SHOWKEY: {
        static int shown = 0;
        shown = !shown;
        SendMessageW(g_edit[ED_KEY], EM_SETPASSWORDCHAR, shown ? 0 : 0x25CF, 0);
        InvalidateRect(g_edit[ED_KEY], NULL, TRUE);
        break;
    }
    case ID_BTN_SCAN:
        scan_devcpp();
        toast(g_devCount ? L"扫描完成" : L"没有找到 Dev-C++，可手动浏览选择");
        InvalidateRect(g_hwnd, NULL, FALSE);
        break;
    case ID_BTN_BROWSE: {
        OPENFILENAMEW ofn;
        wchar_t file[MAX_PATH] = { 0 };
        memset(&ofn, 0, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = g_hwnd;
        ofn.lpstrFilter = L"Dev-C++ 主程序\0DevCpp.exe\0可执行文件\0*.exe\0\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&ofn)) {
            wchar_t root[MAX_PATH];
            wchar_t *p;
            wcsncpy(root, file, MAX_PATH - 1);
            root[MAX_PATH - 1] = 0;
            p = wcsrchr(root, L'\\');
            if (p) *p = 0;
            add_dev(root);
            if (g_devCount > 0) {
                g_devSel = g_devCount - 1;
                toast(L"已添加：%ls", g_dev[g_devSel].root);
            }
            InvalidateRect(g_hwnd, NULL, FALSE);
        }
        break;
    }
    case ID_BTN_INSTALL:
        if (g_devCount <= 0) { toast(L"先选择 Dev-C++ 目录"); break; }
        if (devcpp_running()) { toast(L"请先关闭 Dev-C++ 再接入"); break; }
        if (!is_elevated()) {
            wchar_t args[1024];
            swprintf(args, ARRAYSZ(args), L"--install \"%ls\"", g_dev[g_devSel].root);
            request_elevated(args);
            toast(L"已请求管理员权限，请在弹窗里点「是」");
        } else {
            start_install(g_dev[g_devSel].root);
        }
        break;
    case ID_BTN_REMOVE:
        if (devcpp_running()) { toast(L"请先关闭 Dev-C++ 再还原"); break; }
        if (!is_elevated()) {
            request_elevated(L"--uninstall");
            toast(L"已请求管理员权限，请在弹窗里点「是」");
        } else {
            start_uninstall();
        }
        break;
    case ID_BTN_OPENDEV:
        if (g_devCount > 0) spawn_ex(g_dev[g_devSel].exe, NULL, g_dev[g_devSel].root, 1);
        else toast(L"还没有选择 Dev-C++");
        break;
    case ID_BTN_SAVE_PORT:
        do_save(0);
        toast(L"端口已保存，重启助手服务后生效");
        break;
    case ID_BTN_OPENDIR:
        open_path(g_home);
        break;
    case ID_BTN_OPENCFG: {
        wchar_t p[MAX_PATH];
        swprintf(p, ARRAYSZ(p), L"%ls\\config.json", g_home);
        open_path(p);
        break;
    }
    case ID_BTN_OPENLOG: {
        wchar_t p[MAX_PATH];
        swprintf(p, ARRAYSZ(p), L"%ls\\devai-install.log", g_home);
        if (file_exists(p)) open_path(p);
        else toast(L"还没有日志：先做一次接入或还原");
        break;
    }
    case ID_BTN_RESTART:
        kill_server();
        Sleep(120);
        start_server();
        toast(L"助手服务正在重启…");
        break;
    case ID_BTN_RESET:
        if (port_open(g_cfg.port)) {
            char *resp = NULL;
            http_call(L"POST", L"/api/reset", "{}", &resp, 3000);
            if (resp) free(resp);
            toast(L"已清空上一轮分析记录");
        } else {
            toast(L"助手服务未启动，无需清空");
        }
        break;
    case ID_BTN_GITHUB:
        open_path(L"https://github.com/Qs277/devai-devcpp");
        break;
    case ID_BTN_ISSUE:
        open_path(L"https://github.com/Qs277/devai-devcpp/issues");
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ 字体 / 窗口 */
static void make_fonts(void)
{
    if (fH1) DeleteObject(fH1);
    if (fH2) DeleteObject(fH2);
    if (fTitle) DeleteObject(fTitle);
    if (fBody) DeleteObject(fBody);
    if (fSmall) DeleteObject(fSmall);
    if (fNav) DeleteObject(fNav);
    if (fMono) DeleteObject(fMono);
    if (fNum) DeleteObject(fNum);

    fH1 = CreateFontW(-SC(23), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                      OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                      DEFAULT_PITCH, L"Microsoft YaHei UI");
    fH2 = CreateFontW(-SC(20), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                      OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                      DEFAULT_PITCH, L"Microsoft YaHei UI");
    fTitle = CreateFontW(-SC(15), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH, L"Microsoft YaHei UI");
    fBody = CreateFontW(-SC(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                        OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH, L"Microsoft YaHei UI");
    fSmall = CreateFontW(-SC(11), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH, L"Microsoft YaHei UI");
    fNav = CreateFontW(-SC(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH, L"Microsoft YaHei UI");
    fMono = CreateFontW(-SC(11), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                        OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        FIXED_PITCH, L"Consolas");
    fNum = CreateFontW(-SC(11), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH, L"Microsoft YaHei UI");
    g_fontUi = fBody;
}

static void sync_edit_text(void)
{
    SetWindowTextW(g_edit[ED_BASE], g_cfg.apiBase);
    SetWindowTextW(g_edit[ED_KEY], g_cfg.apiKey);
    SetWindowTextW(g_edit[ED_MODEL], g_cfg.model);
    {
        wchar_t tmp[32];
        swprintf(tmp, ARRAYSZ(tmp), L"%.2f", g_cfg.temperature);
        SetWindowTextW(g_edit[ED_TEMP], tmp);
        swprintf(tmp, ARRAYSZ(tmp), L"%d", g_cfg.maxTokens);
        SetWindowTextW(g_edit[ED_TOK], tmp);
        swprintf(tmp, ARRAYSZ(tmp), L"%d", g_cfg.port);
        SetWindowTextW(g_edit[ED_PORT], tmp);
    }
}

static void create_edits(void)
{
    int i;
    for (i = 0; i < ED_COUNT; i++) {
        g_edit[i] = CreateWindowExW(0, L"EDIT", L"",
                                    WS_CHILD | ES_AUTOHSCROLL | WS_TABSTOP,
                                    0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)kEditId[i],
                                    g_inst, NULL);
        SendMessageW(g_edit[i], WM_SETFONT, (WPARAM)fBody, TRUE);
        SendMessageW(g_edit[i], EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELONG(SC(8), SC(6)));
        SendMessageW(g_edit[i], EM_SETCUEBANNER, FALSE, 0);
        SetWindowLongPtrW(g_edit[i], GWLP_USERDATA, i);
    }
    SendMessageW(g_edit[ED_BASE], EM_SETCUEBANNER, TRUE, (LPARAM)L"https://api.deepseek.com");
    SendMessageW(g_edit[ED_KEY], EM_SETCUEBANNER, TRUE, (LPARAM)L"sk-...");
    SendMessageW(g_edit[ED_KEY], EM_SETPASSWORDCHAR, 0x25CF, 0);
    SendMessageW(g_edit[ED_MODEL], EM_SETCUEBANNER, TRUE, (LPARAM)L"deepseek-chat");
    SendMessageW(g_edit[ED_TEMP], EM_SETCUEBANNER, TRUE, (LPARAM)L"0.2");
    SendMessageW(g_edit[ED_TOK], EM_SETCUEBANNER, TRUE, (LPARAM)L"700");
    SendMessageW(g_edit[ED_PORT], EM_SETCUEBANNER, TRUE, (LPARAM)L"47901");
}

static void apply_dark_title(HWND h)
{
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm) {
        typedef HRESULT(WINAPI *PFN)(HWND, DWORD, LPCVOID, DWORD);
        PFN fn = (PFN)GetProcAddress(dwm, "DwmSetWindowAttribute");
        if (fn) {
            BOOL dark = FALSE;
            DWORD cap = RGB(245, 246, 247);
            DWORD txt = RGB(26, 31, 35);
            DWORD bd = RGB(212, 216, 220);
            fn(h, 20, &dark, sizeof(dark));
            fn(h, 19, &dark, sizeof(dark));
            fn(h, 35, &cap, sizeof(cap));
            fn(h, 36, &txt, sizeof(txt));
            fn(h, 34, &bd, sizeof(bd));
        }
    }
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_hwnd = h;
        make_fonts();
        create_edits();
        sync_edit_text();
        apply_dark_title(h);
        SetTimer(h, 1, 2000, NULL);
        SetTimer(h, 2, 60, NULL);
        scan_devcpp();
        if (payload_needs_extract()) {
            g_extracting = 1;
            CloseHandle(CreateThread(NULL, 0, extract_thread, NULL, 0, NULL));
        } else {
            g_extracting = 0;
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        HDC mem;
        HBITMAP bmp, oldb;
        RECT rc;
        GetClientRect(h, &rc);
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        oldb = (HBITMAP)SelectObject(mem, bmp);
        {
            layout_edits();
            draw_all(mem);
            layout_edits();
            place_edits();
        }
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldb);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEMOVE: {
        POINT pt;
        int id;
        pt.x = GET_X_LPARAM(lp);
        pt.y = GET_Y_LPARAM(lp);
        id = hit_test(pt);
        if (id != g_hover) {
            g_hover = id;
            InvalidateRect(h, NULL, FALSE);
        }
        {
            TRACKMOUSEEVENT tme;
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = h;
            tme.dwHoverTime = 0;
            TrackMouseEvent(&tme);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        if (g_hover != -1) {
            g_hover = -1;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN: {
        POINT pt;
        int id;
        pt.x = GET_X_LPARAM(lp);
        pt.y = GET_Y_LPARAM(lp);
        id = hit_test(pt);
        g_pressed = id;
        if (id < 0) SetFocus(h);
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT pt;
        int id;
        pt.x = GET_X_LPARAM(lp);
        pt.y = GET_Y_LPARAM(lp);
        id = hit_test(pt);
        if (id >= 0 && id == g_pressed) on_click(id);
        g_pressed = -1;
        return 0;
    }

    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, C_TEXT);
        SetBkColor(dc, C_INPUT);
        return (LRESULT)brInput;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);
        int code = HIWORD(wp);
        if (code == EN_SETFOCUS || code == EN_KILLFOCUS) {
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        if (id == kEditId[ED_PORT]) {
            /* 端口框只留数字 */
            if (code == EN_CHANGE) {
                wchar_t buf[32];
                wchar_t clean[32];
                int i, n = 0;
                GetWindowTextW(g_edit[ED_PORT], buf, ARRAYSZ(buf));
                for (i = 0; buf[i] && n < 10; i++)
                    if (buf[i] >= L'0' && buf[i] <= L'9') clean[n++] = buf[i];
                clean[n] = 0;
                if (wcscmp(buf, clean) != 0) SetWindowTextW(g_edit[ED_PORT], clean);
            }
        }
        return 0;
    }

    case WM_TIMER:
        if (wp == 1) {
            int was = g_serverUp;
            g_serverUp = port_open(g_cfg.port);
            if (was != g_serverUp) InvalidateRect(h, NULL, FALSE);
        } else if (wp == 2) {
            if (g_extracting || g_busy) {
                g_spin = (g_spin + 30) % 360;
                InvalidateRect(h, NULL, FALSE);
            }
            if (GetTickCount64() >= g_toastUntil && g_toast[0]) {
                g_toast[0] = 0;
                InvalidateRect(h, NULL, FALSE);
            }
        } else if (wp == 3) {
            if (g_busyProc && WaitForSingleObject(g_busyProc, 0) == WAIT_OBJECT_0) {
                DWORD code = 0;
                GetExitCodeProcess(g_busyProc, &code);
                CloseHandle(g_busyProc);
                g_busyProc = NULL;
                g_busy = 0;
                KillTimer(h, 3);
                scan_devcpp();
                InvalidateRect(h, NULL, FALSE);
                if (code == 0) {
                    toast(L"接入完成，可以点「开始使用」了");
                    if (g_cliAuto) SetTimer(h, 4, 1100, NULL);
                } else {
                    toast(L"操作没有完成，点「查看日志」看看原因");
                }
            }
        } else if (wp == 4) {
            KillTimer(h, 4);
            PostMessageW(h, WM_CLOSE, 0, 0);
        }
        return 0;

    case WM_APP + 1: {
        /* 解包结束 */
        int ok = (int)wp;
        g_extracting = 0;
        if (!ok) {
            g_extractFailed = 1;
            toast(L"组件释放失败：%ls", g_extractMsg);
        } else {
            g_extractFailed = 0;
            config_load();
            sync_edit_text();
            toast(L"组件已就绪");
        }
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }

    case WM_APP + 2:
        g_testing = 0;
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_APP + 3:
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE) {
            g_elevated = is_elevated();
            scan_devcpp();
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;

    case WM_DPICHANGED: {
        RECT *nr = (RECT *)lp;
        g_dpi = HIWORD(wp);
        make_fonts();
        SetWindowPos(h, NULL, nr->left, nr->top, nr->right - nr->left, nr->bottom - nr->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void init_window(int dpi)
{
    WNDCLASSEXW wc;
    RECT rc;
    DWORD style = WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX;
    int w = SC(940), hh = SC(640);
    int sw, sh;

    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = g_inst;
    wc.hIcon = icoBig;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszMenuName = NULL;
    wc.lpszClassName = L"DevAICenterWnd";
    wc.hIconSm = icoSmall;
    RegisterClassExW(&wc);

    rc.left = 0; rc.top = 0; rc.right = w; rc.bottom = hh;
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    sw = rc.right - rc.left;
    sh = rc.bottom - rc.top;

    g_hwnd = CreateWindowExW(0, L"DevAICenterWnd", L"小D · DevAI 控制台",
                             style, (GetSystemMetrics(SM_CXSCREEN) - sw) / 2,
                             (GetSystemMetrics(SM_CYSCREEN) - sh) / 2,
                             sw, sh, NULL, NULL, g_inst, NULL);
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    UpdateWindow(g_hwnd);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    WSADATA wsa;
    MSG msg;
    int argc = 0;
    wchar_t **argv;
    int cliInstall = 0, cliUninstall = 0;
    wchar_t cliRoot[MAX_PATH] = { 0 };

    (void)prev;
    (void)show;
    g_inst = inst;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        int i;
        for (i = 1; i < argc; i++) {
            if (_wcsicmp(argv[i], L"--install") == 0) {
                cliInstall = 1;
                if (i + 1 < argc) { wcsncpy(cliRoot, argv[i + 1], MAX_PATH - 1); i++; }
            } else if (_wcsicmp(argv[i], L"--uninstall") == 0) {
                cliUninstall = 1;
            }
        }
        LocalFree(argv);
    }
    (void)cmd;

    /* 单文件运行目录 */
    GetModuleFileNameW(NULL, g_exePath, MAX_PATH);
    {
        wchar_t dir[MAX_PATH], marker[MAX_PATH];
        wcscpy(dir, g_exePath);
        {
            wchar_t *p = wcsrchr(dir, L'\\');
            if (p) *p = 0;
        }
        swprintf(marker, ARRAYSZ(marker), L"%ls\\portable.txt", dir);
        if (file_exists(marker)) {
            swprintf(g_home, ARRAYSZ(g_home), L"%ls\\DevAI-Data", dir);
        } else {
            wchar_t ad[MAX_PATH] = { 0 };
            if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, ad)) || !ad[0]) {
                GetEnvironmentVariableW(L"LOCALAPPDATA", ad, MAX_PATH);
            }
            swprintf(g_home, ARRAYSZ(g_home), L"%ls\\DevAI", ad);
        }
        ensure_dir(g_home);
    }

    g_elevated = is_elevated();
    config_load();
    {
        wchar_t cfgp[MAX_PATH];
        swprintf(cfgp, ARRAYSZ(cfgp), L"%ls\\config.json", g_home);
        if (!file_exists(cfgp)) config_save();
    }

    icoApp = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                               GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    icoSmall = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    icoBig = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 128, 128, 0);

    brBg = CreateSolidBrush(C_BG);
    brSide = CreateSolidBrush(C_SIDE);
    brCard = CreateSolidBrush(C_CARD);
    brInput = CreateSolidBrush(C_INPUT);

    /* DPI */
    {
        HMODULE u = GetModuleHandleW(L"user32.dll");
        if (u) {
            typedef UINT(WINAPI *PFN)(void);
            PFN fn = (PFN)GetProcAddress(u, "GetDpiForSystem");
            if (fn) g_dpi = (int)fn();
        }
        if (g_dpi < 96) g_dpi = 96;
    }

    init_window(g_dpi);
    if (cliInstall) {
        g_cliAuto = 1;
        if (cliRoot[0]) start_install(cliRoot);
    } else if (cliUninstall) {
        g_cliAuto = 1;
        start_uninstall();
    }

    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (!IsDialogMessageW(g_hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    WSACleanup();
    return 0;
}
