/*
 * devai_cc.c —— DevAI 编译器前端（夹在 Dev-C++ 和真正的 gcc/g++ 之间）
 *
 * 作用：
 *   1. 收到 Dev-C++ 发来的编译参数后，转发给同目录下真正的 gcc.exe / g++.exe；
 *   2. 把编译器的 stdout/stderr 边转发给 Dev-C++（保证它原本的报错列表照常工作），
 *      边写入日志文件；
 *   3. 把"开始编译 / 编译结束（退出码）"写进 D:\devai\queue 的事件队列；
 *   4. 如果 AI 助手进程没开，自动在后台拉起它。
 *
 * 编译（用 Dev-C++ 自带的 MinGW）：
 *   gcc.exe -O2 -s devai_cc.c -o devai_gcc.exe
 * 然后把同一个 exe 复制成 devai_gpp.exe 即可（按文件名区分走 gcc 还是 g++）。
 */

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DEVAI_HOME_DEFAULT
#define DEVAI_HOME_DEFAULT "D:\\devai"
#endif

static char g_home[MAX_PATH];
static char g_log[MAX_PATH];
static char g_mode[8];   /* "gcc" 或 "gpp" */

/* 把 ANSI(GBK) 字符串转成 UTF-8，写事件文件时用，避免中文路径乱码 */
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

static void kv_num(FILE *f, const char *key, long long val)
{
    fprintf(f, "%s=%lld\r\n", key, val);
}

static unsigned long long now_ms(void)
{
    FILETIME ft;
    ULONGLONG t;
    GetSystemTimeAsFileTime(&ft);
    t = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return t / 10000ULL - 11644473600000ULL; /* 转成 Unix 毫秒 */
}

/* 生成事件文件名：queue\evt_<时间戳>_<pid>_<kind>.kv */
static void event_name(char *out, const char *kind)
{
    sprintf(out, "%s\\queue\\evt_%llu_%lu_%s.kv", g_home, now_ms(), GetCurrentProcessId(), kind);
}

/* 判断 AI 助手是否活着（读 agent.pid） */
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

/* 后台上拉 AI 助手（隐藏窗口，不阻塞编译） */
static void ensure_agent(void)
{
    char path[MAX_PATH * 3], node[MAX_PATH] = "node.exe", srv[MAX_PATH];
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

/* 拼接命令行：把参数重新加上引号（含空格的路径必须引号，否则 gcc 会拆断） */
static void build_command(char *out, int cap, const char *exe, int argc, char **argv)
{
    int i, len;
    out[0] = 0;
    sprintf(out, "\"%s\"", exe);
    len = (int)strlen(out);
    for (i = 1; i < argc && len < cap - 1; i++) {
        const char *a = argv[i];
        /* 含空格才需要补引号；已经带引号的参数原样透传，避免二次包装 */
        int needq = (strchr(a, ' ') != NULL) && (strchr(a, '"') == NULL);
        if (len + 1 < cap) out[len++] = ' ';
        if (needq && len + 1 < cap) out[len++] = '"';
        while (*a && len < cap - 1) out[len++] = *a++;
        if (needq && len + 1 < cap) out[len++] = '"';
        out[len] = 0;
    }
    out[cap - 1] = 0;
}

/* 在候选名字里找真正的编译器（避免找到自己造成无限递归） */
static int resolve_real(const char *dir, const char *mode, char *out)
{
    char self[MAX_PATH], cand[MAX_PATH];
    const char *names[2];
    int i;

    GetModuleFileNameA(NULL, self, MAX_PATH);
    names[0] = (strcmp(mode, "gpp") == 0) ? "g++-real.exe" : "gcc-real.exe";
    names[1] = (strcmp(mode, "gpp") == 0) ? "g++.exe" : "gcc.exe";

    for (i = 0; i < 2; i++) {
        sprintf(cand, "%s\\%s", dir, names[i]);
        if (GetFileAttributesA(cand) == INVALID_FILE_ATTRIBUTES) continue;
        if (_stricmp(cand, self) == 0) continue;
        lstrcpynA(out, cand, MAX_PATH);
        return 1;
    }
    return 0;
}

/* 从参数里挑出编译目标（-o xxx）和源文件，写进事件，助手据此读源码 */
static void write_args(FILE *f, int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-o") == 0 && i + 1 < argc) {
            kv(f, "out", argv[++i]);
            continue;
        }
        if (strcmp(a, "-c") == 0) kv(f, "flag_c", "1");
        kv(f, "arg", a);
    }
}

int main(int argc, char **argv)
{
    SECURITY_ATTRIBUTES sa;
    HANDLE outR = NULL, outW = NULL, hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char dir[MAX_PATH], self[MAX_PATH], realexe[MAX_PATH];
    char cmd[MAX_PATH * 4];
    char *p, *name;
    FILE *logf, *ef;
    unsigned long long t0;
    DWORD code = 1, n = 0, wr = 0;
    int st;
    char efile[MAX_PATH];

    /* ---- 1. 基本环境 ---- */
    if (GetEnvironmentVariableA("DEVAI_HOME", g_home, MAX_PATH) <= 0)
        lstrcpynA(g_home, DEVAI_HOME_DEFAULT, MAX_PATH);

    GetModuleFileNameA(NULL, self, MAX_PATH);
    lstrcpynA(dir, self, MAX_PATH);
    p = strrchr(dir, '\\');
    if (p) *p = 0;

    name = strrchr(self, '\\');
    name = name ? name + 1 : self;
    lstrcpynA(g_mode, (strstr(name, "gpp") || strstr(name, "g++")) ? "gpp" : "gcc", 8);

    if (!resolve_real(dir, g_mode, realexe)) {
        fprintf(stderr, "[DevAI] cannot locate real compiler in %s\n", dir);
        return 1;
    }

    /* ---- 2. 准备日志与事件 ---- */
    CreateDirectoryA(g_home, NULL);
    sprintf(g_log, "%s\\queue", g_home);
    CreateDirectoryA(g_log, NULL);
    sprintf(g_log, "%s\\queue\\log_%llu_%lu.txt", g_home, now_ms(), GetCurrentProcessId());

    ensure_agent();

    event_name(efile, "cs");
    ef = fopen(efile, "wb");
    if (ef) {
        char cwd[MAX_PATH];
        GetCurrentDirectoryA(MAX_PATH, cwd);
        kv(ef, "kind", "compile_start");
        kv(ef, "compiler", g_mode);
        kv_num(ef, "ts", (long long)now_ms());
        kv_num(ef, "pid", (long long)GetCurrentProcessId());
        kv(ef, "cwd", cwd);
        kv(ef, "log", g_log);
        kv(ef, "exe", realexe);
        write_args(ef, argc, argv);
        fclose(ef);
    }

    /* ---- 3. 建立管道，启动真正的编译器 ---- */
    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&outR, &outW, &sa, 0)) {
        fprintf(stderr, "[DevAI] CreatePipe failed\n");
        return 1;
    }
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);

    build_command(cmd, sizeof(cmd), realexe, argc, argv);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = outW;
    si.hStdError = outW;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));

    logf = fopen(g_log, "wb");

    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        DWORD err = GetLastError();
        if (logf) fclose(logf);
        CloseHandle(outR);
        CloseHandle(outW);
        fprintf(stderr, "[DevAI] failed to start compiler (error %lu): %s\n", err, cmd);
        event_name(efile, "ce");
        ef = fopen(efile, "wb");
        if (ef) {
            kv(ef, "kind", "compile_end");
            kv(ef, "compiler", g_mode);
            kv(ef, "log", g_log);
            kv(ef, "spawn_failed", "1");
            kv_num(ef, "exit", 1);
            fclose(ef);
        }
        return 1;
    }

    CloseHandle(pi.hThread);
    CloseHandle(outW); /* 父进程必须关掉写端，否则读端永远等不到 EOF */

    /* ---- 4. 一边读一边转发 + 落盘 ---- */
    t0 = now_ms();
    for (;;) {
        char buf[8192];
        BOOL ok = ReadFile(outR, buf, sizeof(buf), &n, NULL);
        if (!ok || n == 0) break;
        if (logf) { fwrite(buf, 1, n, logf); fflush(logf); }
        if (hOut) WriteFile(hOut, buf, n, &wr, NULL);
    }
    CloseHandle(outR);

    WaitForSingleObject(pi.hProcess, INFINITE);
    if (!GetExitCodeProcess(pi.hProcess, &code)) code = 1;
    CloseHandle(pi.hProcess);

    if (logf) fclose(logf);

    /* ---- 5. 写结束事件，返回真实退出码给 Dev-C++ ---- */
    event_name(efile, "ce");
    ef = fopen(efile, "wb");
    if (ef) {
        kv(ef, "kind", "compile_end");
        kv(ef, "compiler", g_mode);
        kv(ef, "log", g_log);
        kv_num(ef, "exit", (long long)code);
        kv_num(ef, "ms", (long long)(now_ms() - t0));
        kv_num(ef, "pid", (long long)GetCurrentProcessId());
        fclose(ef);
    }

    st = (int)code;
    return st;
}
