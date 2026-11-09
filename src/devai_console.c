/*
 * devai_console.c —— 增强版 ConsolePauser（替换 Dev-C++ 安装目录下的 ConsolePauser.exe）
 *
 * 行为与原版保持一致：运行 <program>，输出实时回显，结束后 "Process exited after ..."
 * 并暂停等待按键。额外做的事情：
 *   - 把退出码、耗时、程序路径写进 D:\devai\queue 事件队列；
 *   - 异常退出（崩溃/栈溢出/除零）时给出提示，并确保 AI 助手已启动。
 *
 * 编译：gcc.exe -O2 -s devai_console.c -o devai_console.exe
 */

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifndef DEVAI_HOME_DEFAULT
#define DEVAI_HOME_DEFAULT "D:\\devai"
#endif

#define MAX_COMMAND_LENGTH 32768

static char g_home[MAX_PATH];

/* 装到非默认目录时，安装器会在本 exe 旁边写 devai-home.txt，
   没有 DEVAI_HOME 环境变量时用它定位 HOME */
static int home_from_marker(const char *dir, char *out, int cap)
{
    char p[MAX_PATH], buf[MAX_PATH];
    FILE *f;
    int i, k;
    sprintf(p, "%s\\devai-home.txt", dir);
    f = fopen(p, "rb");
    if (!f) return 0;
    buf[0] = 0;
    fgets(buf, sizeof(buf), f);
    fclose(f);
    for (i = 0; buf[i]; i++) {
        if (buf[i] == '\r' || buf[i] == '\n') { buf[i] = 0; break; }
    }
    for (k = 0; buf[k] == ' ' || buf[k] == '\t'; k++) {}
    if (!buf[k]) return 0;
    lstrcpynA(out, buf + k, cap);
    return 1;
}

static void to_utf8(const char *in, char *out, int outlen)
{
    int wlen;
    wchar_t *w;
    if (!in) in = "";
    wlen = MultiByteToWideChar(CP_ACP, 0, in, -1, NULL, 0);
    if (wlen <= 0) { lstrcpynA(out, in, outlen); return; }
    w = (wchar_t *)malloc(sizeof(wchar_t) * wlen);
    if (!w) { lstrcpynA(out, in, outlen); return; }
    MultiByteToWideChar(CP_ACP, 0, in, -1, w, wlen);
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, outlen, NULL, NULL))
        lstrcpynA(out, in, outlen);
    free(w);
}

static void kv(FILE *f, const char *key, const char *val)
{
    char u8[MAX_PATH * 3];
    to_utf8(val, u8, sizeof(u8));
    fprintf(f, "%s=%s\r\n", key, u8);
}

static unsigned long long now_ms(void)
{
    FILETIME ft;
    ULONGLONG t;
    GetSystemTimeAsFileTime(&ft);
    t = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return t / 10000ULL - 11644473600000ULL;
}

static LONGLONG tick(void)
{
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

static LONGLONG freq(void)
{
    LARGE_INTEGER v;
    QueryPerformanceFrequency(&v);
    return v.QuadPart;
}

static int agent_alive(void)
{
    char path[MAX_PATH];
    FILE *f;
    unsigned pid = 0;
    HANDLE h;
    DWORD code = 0;
    int alive = 0;

    sprintf(path, "%s\\agent.pid", g_home);
    f = fopen(path, "rb");
    if (!f) return 0;
    if (fscanf(f, "%u", &pid) != 1 || pid == 0) { fclose(f); return 0; }
    fclose(f);

    h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return 0;
    if (GetExitCodeProcess(h, &code) && code == STILL_ACTIVE) alive = 1;
    CloseHandle(h);
    return alive;
}

static void ensure_agent(void)
{
    char path[MAX_PATH * 3];
    char node[MAX_PATH] = "node.exe", srv[MAX_PATH];
    FILE *f;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    if (getenv("DEVAI_NO_AUTOSTART")) return;
    if (agent_alive()) return;

    sprintf(path, "%s\\node_path.txt", g_home);
    f = fopen(path, "rb");
    if (f) {
        char line[MAX_PATH] = {0};
        if (fgets(line, sizeof(line), f)) {
            int n = (int)strlen(line);
            while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n' || line[n - 1] == ' '))
                line[--n] = 0;
            if (n > 0) lstrcpynA(node, line, MAX_PATH);
        }
        fclose(f);
    }

    sprintf(srv, "%s\\server\\server.js", g_home);
    {
        char cmd[MAX_PATH * 3];
        sprintf(cmd, "\"%s\" \"%s\"", node, srv);
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        ZeroMemory(&pi, sizeof(pi));
        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL,
                           g_home, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
}

static void string_join(char *out, int cap, int argc, char **argv)
{
    int i;
    out[0] = 0;
    for (i = 1; i < argc; i++) {
        int needq = (strchr(argv[i], ' ') != NULL) && (strchr(argv[i], '"') == NULL);
        if (i > 1) strncat(out, " ", cap - (int)strlen(out) - 1);
        if (needq) strncat(out, "\"", cap - (int)strlen(out) - 1);
        strncat(out, argv[i], cap - (int)strlen(out) - 1);
        if (needq) strncat(out, "\"", cap - (int)strlen(out) - 1);
    }
    out[cap - 1] = 0;
}

/* 根据 exe 路径猜对应源码，方便助手读文件 */
static void guess_source(const char *cmd, char *out)
{
    char base[MAX_PATH];
    const char *exts[2] = { ".cpp", ".c" };
    int i;

    out[0] = 0;
    lstrcpynA(base, cmd, MAX_PATH);
    if (strlen(base) > 4) base[strlen(base) - 4] = 0; /* 去掉 .exe */
    for (i = 0; i < 2; i++) {
        char p[MAX_PATH];
        sprintf(p, "%s%s", base, exts[i]);
        if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) {
            lstrcpynA(out, p, MAX_PATH);
            return;
        }
    }
}

static void write_run_event(DWORD code, double secs, const char *cmd, const char *src)
{
    char efile[MAX_PATH];
    FILE *f;

    sprintf(efile, "%s\\queue\\evt_%llu_%lu_run.kv", g_home, now_ms(), GetCurrentProcessId());
    f = fopen(efile, "wb");
    if (!f) return;
    kv(f, "kind", "run_end");
    kv(f, "exe", cmd);
    kv(f, "src", src);
    fprintf(f, "exit=%lu\r\n", code);
    fprintf(f, "ms=%.0f\r\n", secs * 1000.0);
    fprintf(f, "ts=%llu\r\n", now_ms());
    fclose(f);
}

/* Dev-C++ 是最大化窗口，程序的控制台会开在它后面，用户看到的只是“运行之后什么也没出来”。
   这里把控制台窗口抬到最前（Windows 10 老控制台 / Windows 11 Terminal 都适用）。 */
static void console_to_front(const char *title)
{
    WCHAR wtitle[MAX_PATH];
    HWND h = NULL, fg;
    DWORD ft, mt;
    int i;

    MultiByteToWideChar(CP_ACP, 0, title, -1, wtitle, MAX_PATH);
    for (i = 0; i < 30; i++) {
        h = FindWindowW(NULL, wtitle);
        if (!h) h = GetConsoleWindow();
        if (h && IsWindowVisible(h) && !IsIconic(h)) break;
        Sleep(100);
        h = NULL;
    }
    if (!h) return;

    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    ShowWindow(h, SW_SHOW);
    /* Dev-C++ 运行时会把自己藏起来，前台会落到别的置顶窗口（比如助手面板）上，
       所以多盯一秒，看到控制台没在前台就再抬一次 */
    for (i = 0; i < 15; i++) {
        if (GetForegroundWindow() == h) break;
        fg = GetForegroundWindow();
        ft = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
        mt = GetCurrentThreadId();
        if (ft && ft != mt) AttachThreadInput(mt, ft, TRUE);
        SetForegroundWindow(h);
        BringWindowToTop(h);
        if (ft && ft != mt) AttachThreadInput(mt, ft, FALSE);
        Sleep(100);
    }
}

int main(int argc, char **argv)
{
    char command[MAX_COMMAND_LENGTH];
    char src[MAX_PATH];
    LONGLONG start, end;
    double seconds;
    DWORD ret;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char self[MAX_PATH], dir[MAX_PATH];
    char *p;

    GetModuleFileNameA(NULL, self, MAX_PATH);
    lstrcpynA(dir, self, MAX_PATH);
    p = strrchr(dir, '\\');
    if (p) *p = 0;

    if (GetEnvironmentVariableA("DEVAI_HOME", g_home, MAX_PATH) <= 0) {
        if (!home_from_marker(dir, g_home, MAX_PATH))
            lstrcpynA(g_home, DEVAI_HOME_DEFAULT, MAX_PATH);
    }

    if (argc < 2) {
        printf("\n--------------------------------");
        printf("\nUsage: ConsolePauser.exe <filename> <parameters>\n");
        system("pause");
        return 0;
    }

    SetConsoleTitleA(argv[1]);

    string_join(command, MAX_COMMAND_LENGTH, argc, argv);

    ensure_agent();

    start = tick();

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessA(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("\n--------------------------------");
        printf("\nFailed to execute \"%s\": error %lu\n", command, GetLastError());
        system("pause");
        return 1;
    }
    CloseHandle(pi.hThread);
    console_to_front(argv[1]);
    WaitForSingleObject(pi.hProcess, INFINITE);

    ret = 0;
    GetExitCodeProcess(pi.hProcess, &ret);
    CloseHandle(pi.hProcess);

    end = tick();
    seconds = (end - start) / (double)freq();

    guess_source(argv[1], src);
    write_run_event(ret, seconds, argv[1], src);

    printf("\n--------------------------------");
    printf("\nProcess exited after %.4g seconds with return value %lu\n", seconds, ret);

    if (ret != 0) {
        printf("  [DevAI] abnormal exit code detected (%lu).\n", ret);
        printf("  [DevAI] Open the AI assistant panel for a beginner-friendly explanation.\n");
    }

    if (!getenv("DEVAI_NO_PAUSE"))
        system("pause");
    return 0;
}
