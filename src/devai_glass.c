/*
 * devai_glass.exe —— 在 Dev-C++ 界面上盖一层透明玻璃
 *
 *  - 编辑区：出错行画高亮带 + 小D + 虚线箭头 + 可折叠讲解卡
 *  - 下方“编译日志”区：玻璃条，写编译结果 / 错误分析
 *
 *  数据来自 D:\devai\state\glass.json（由 server.js 写入）：
 *  {on,kind,busy|result,ok,level,title,line,name,cause,where,how[],code,tips[],tip,ts}
 *
 *  窗口全部 WS_EX_LAYERED（逐像素 alpha），高亮/玻璃条点击穿透，
 *  讲解卡可以点标题栏折叠、拖动。
 */
#define _WIN32_WINNT 0x0601
#define WINVER 0x0601
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>

#ifndef MODSHIFT
#define MODSHIFT 0
#endif

#define CLASS_GLASS L"DevAIGlassOverlay"

/* ------------------------------------------------------------------ 全局 */

static WCHAR gHome[MAX_PATH] = L"D:\\devai";
static WCHAR gJsonPath[MAX_PATH];
static WCHAR gAlivePath[MAX_PATH];

static double gDpr = 1.0;
static int gDpi = 144;

static HFONT g_fHead, g_fBody, g_fSmall, g_fMono, g_fTag;

static HWND g_hBand, g_hStrip, g_hCard;
static HWND g_hEdit, g_hHost, g_hMemo;

static int g_hostL, g_hostT, g_hostR, g_hostB;
static int g_edL, g_edT, g_edR, g_edB;       /* 编辑区屏幕坐标 */
static int g_memoL, g_memoT, g_memoR, g_memoB;

static int g_pitch;        /* 行高（像素） */
static int g_firstTextY;   /* 编辑区第一行文字的 y（相对编辑区） */
static int g_topLine = 1;  /* 当前第一可见行 */

static int g_collapsed = 1;   /* 默认折叠，只留一条小提示 */
static int g_cardHidden = 0;
static int g_userDX = 0, g_userDY = 0;
static int g_needDraw = 1;
static int g_cardX = 0, g_cardY = 0;
static int g_cardHeadH = 0;   /* 点击标题栏才折叠/展开 */
static int g_inkRight = 0;    /* 可见代码最右侧有字的 x（相对编辑区），卡片放它右边 */
static int g_lastCodeY = 0;   /* 可见代码最后一行的底部（相对编辑区） */
static int g_panelL = 0, g_panelT = 0, g_panelB = 0;  /* 小助手浮窗的位置，卡片别压它 */
static int g_topLineSet = 1;
static char g_glassDoc[8192];   /* 缓存 json 原文 */

static HDC g_measureDC;
static WCHAR gLogPath[MAX_PATH];

static void glog(const WCHAR *fmt, ...)
{
  WCHAR buf[512];
  va_list ap;
  char utf8[1024];
  int n;
  HANDLE f;
  va_start(ap, fmt);
  _vsnwprintf(buf, 500, fmt, ap);
  va_end(ap);
  buf[500] = 0;
  n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, 1000, NULL, NULL);
  if (n <= 1) return;
  utf8[n - 1] = '\r'; utf8[n] = '\n';
  f = CreateFileW(gLogPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                  NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE) return;
  DWORD w = 0;
  WriteFile(f, utf8, (DWORD)n + 1, &w, NULL);
  CloseHandle(f);
}

static int g_dragStartX, g_dragStartY, g_dragging = 0;
static RECT g_dragRect;

/* 数据 */
typedef struct {
  int hasData;
  int on;
  WCHAR kind[16];
  int ok;
  WCHAR level[24];
  WCHAR title[160];
  WCHAR cause[520];
  WCHAR where[300];
  WCHAR code[1200];
  WCHAR tip[300];
  WCHAR bad[300];
  WCHAR good[300];
  WCHAR name[160];
  WCHAR how[600];
  WCHAR tips[300];
  int line;
  DWORD ts;
} GlassData;

static GlassData gd;

/* ------------------------------------------------------------------ 画布 */

typedef struct {
  int w, h;
  HDC hdc;
  HBITMAP bmp;
  void *bits;
  unsigned int *px;
} Canvas;

static int S(int px) { return (int)(px * gDpr + 0.5); }

static BOOL cvCreate(Canvas *c, int w, int h)
{
  BITMAPINFO bi;
  HDC screen;
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  screen = GetDC(NULL);
  c->hdc = CreateCompatibleDC(screen);
  ReleaseDC(NULL, screen);
  ZeroMemory(&bi, sizeof(bi));
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;   /* 自上而下 */
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  c->bmp = CreateDIBSection(c->hdc, &bi, DIB_RGB_COLORS, &c->bits, NULL, 0);
  if (!c->bmp) { DeleteDC(c->hdc); return FALSE; }
  SelectObject(c->hdc, c->bmp);
  c->w = w; c->h = h;
  c->px = (unsigned int *)c->bits;
  memset(c->bits, 0, (size_t)w * h * 4);
  return TRUE;
}

static void cvFree(Canvas *c)
{
  if (c->bmp) DeleteObject(c->bmp);
  if (c->hdc) DeleteDC(c->hdc);
  c->bmp = NULL; c->hdc = NULL; c->px = NULL;
}

static void cvBlend(Canvas *c, int x, int y, int r, int g, int b, int a)
{
  unsigned int *p;
  int k, nr, ng, nb, na, er, eg, eb, ea;
  unsigned int v;
  if (a <= 0) return;
  if (a > 255) a = 255;
  if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
  p = &c->px[(size_t)y * c->w + x];
  v = *p;
  ea = (v >> 24) & 255; er = (v >> 16) & 255; eg = (v >> 8) & 255; eb = v & 255;
  k = 255 - a;
  nr = r * a / 255 + er * k / 255;
  ng = g * a / 255 + eg * k / 255;
  nb = b * a / 255 + eb * k / 255;
  na = a + ea * k / 255;
  *p = ((unsigned)na << 24) | ((unsigned)nr << 16) | ((unsigned)ng << 8) | (unsigned)nb;
}

static void cvFill(Canvas *c, int x, int y, int w, int h, int r, int g, int b, int a)
{
  int i, j;
  for (j = 0; j < h; j++)
    for (i = 0; i < w; i++)
      cvBlend(c, x + i, y + j, r, g, b, a);
}

static void cvRect(Canvas *c, int x, int y, int w, int h, int r, int g, int b, int a)
{
  cvFill(c, x, y, w, 1, r, g, b, a);
  cvFill(c, x, y + h - 1, w, 1, r, g, b, a);
  cvFill(c, x, y, 1, h, r, g, b, a);
  cvFill(c, x + w - 1, y, 1, h, r, g, b, a);
}

static void cvLine(Canvas *c, int x1, int y1, int x2, int y2, int r, int g, int b, int a)
{
  int dx = x2 - x1, dy = y2 - y1;
  int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
  int i;
  if (steps < 1) steps = 1;
  for (i = 0; i <= steps; i++) {
    cvBlend(c, x1 + dx * i / steps, y1 + dy * i / steps, r, g, b, a);
    cvBlend(c, x1 + dx * i / steps, y1 + dy * i / steps + 1, r, g, b, a);
  }
}

/* 自己画小三角：字体里没有 ▸/▾ 这类符号时不会变成豆腐块 */
static void cvChevron(Canvas *c, int x, int y, int s, int down, int r, int g, int b, int a)
{
  if (down) {
    cvLine(c, x, y, x + s, y + s, r, g, b, a);
    cvLine(c, x + s, y + s, x + 2 * s, y, r, g, b, a);
  } else {
    cvLine(c, x, y, x + s, y + s, r, g, b, a);
    cvLine(c, x + s, y + s, x, y + 2 * s, r, g, b, a);
  }
}

static void cvDashedLine(Canvas *c, int x1, int y1, int x2, int y2, int r, int g, int b, int a, int dash)
{
  int dx = x2 - x1, dy = y2 - y1;
  int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
  int i;
  if (steps < 1) steps = 1;
  for (i = 0; i <= steps; i++) {
    if ((i / dash) % 2 == 1) continue;
    cvBlend(c, x1 + dx * i / steps, y1 + dy * i / steps, r, g, b, a);
    cvBlend(c, x1 + dx * i / steps, y1 + dy * i / steps + 1, r, g, b, a);
  }
}

/* 文字：先用 GDI 画到黑白 mask，再按灰度做 alpha 合成（有抗锯齿） */
static int textW(HDC dc, HFONT f, const WCHAR *s)
{
  SIZE sz;
  HFONT old = SelectObject(dc, f);
  GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
  SelectObject(dc, old);
  return sz.cx;
}

static int textLineH(HDC dc, HFONT f)
{
  TEXTMETRICW tm;
  HFONT old = SelectObject(dc, f);
  GetTextMetricsW(dc, &tm);
  SelectObject(dc, old);
  return tm.tmHeight + tm.tmExternalLeading;
}

static void drawTextAt(Canvas *c, HDC measure, int x, int y, const WCHAR *s,
                       HFONT f, int r, int g, int b, int a)
{
  SIZE sz;
  Canvas mask;
  int i, j, w, h;
  unsigned int *mp;
  if (!s || !s[0]) return;
  SelectObject(measure, f);
  GetTextExtentPoint32W(measure, s, (int)wcslen(s), &sz);
  w = sz.cx + 2; h = sz.cy + 2;
  if (w < 2 || h < 2 || w > 4000 || h > 400) return;
  if (!cvCreate(&mask, w, h)) return;
  SetBkMode(mask.hdc, TRANSPARENT);
  SetTextColor(mask.hdc, RGB(255, 255, 255));
  SelectObject(mask.hdc, f);
  TextOutW(mask.hdc, 1, 1, s, (int)wcslen(s));
  mp = mask.px;
  for (j = 0; j < h; j++) {
    for (i = 0; i < w; i++) {
      unsigned int v = mp[(size_t)j * w + i];
      int cov = (v >> 8) & 255;   /* 白字，取绿通道 */
      if (cov > 0) cvBlend(c, x + i, y + j, r, g, b, cov * a / 255);
    }
  }
  cvFree(&mask);
}

/* 按宽度换行（中英混排，尽量在空格处断） */
static int wrapLines(HDC dc, HFONT f, const WCHAR *src, int maxw,
                     WCHAR *out, int outCap, int maxLines, int lineChars)
{
  int n = 0, pos = 0, len = (int)wcslen(src);
  if (!src) return 0;
  while (pos < len && n < maxLines) {
    WCHAR buf[1024];
    int bi = 0, lastSpace = -1, k;
    int cur = 0;
    WCHAR piece[1024];
    while (pos < len) {
      WCHAR ch = src[pos];
      if (ch == '\r') { pos++; continue; }
      if (ch == '\n') { pos++; break; }
      piece[0] = ch; piece[1] = 0;
      cur = textW(dc, f, piece);
      if (bi > 0 && (textW(dc, f, buf) + cur) > maxw) break;
      if (bi >= lineChars - 1) break;
      buf[bi++] = ch;
      buf[bi] = 0;
      if (ch == ' ') lastSpace = bi;
      pos++;
    }
    if (bi == 0 && pos < len) { buf[bi++] = src[pos++]; buf[bi] = 0; }
    /* 在空格处断行更自然 */
    if (pos < len && lastSpace > 0 && bi > lastSpace + 2) {
      int back = bi - lastSpace;
      bi = lastSpace;
      buf[bi] = 0;
      pos -= back;
    }
    /* 去掉行首空格 */
    for (k = 0; k < bi && buf[k] == ' '; k++) {}
    if (bi - k > 0) {
      wcsncpy(out + n * outCap, buf + k, outCap - 1);
      out[n * outCap + outCap - 1] = 0;
      n++;
    }
  }
  return n;
}

/* 单行裁剪：超出宽度加省略号（简单二分） */
static void copyEllipsis(HDC dc, HFONT f, const WCHAR *src, int maxw, WCHAR *out, int cap)
{
  int len = (int)wcslen(src);
  int lo = 0, hi = len;
  out[0] = 0;
  if (len <= 0) return;
  if (textW(dc, f, src) <= maxw) {
    wcsncpy(out, src, cap - 1); out[cap - 1] = 0;
    return;
  }
  while (lo < hi) {
    int mid = (lo + hi + 1) / 2;
    WCHAR tmp[1024];
    if (mid >= cap) mid = cap - 1;
    wcsncpy(tmp, src, mid); tmp[mid] = 0;
    wcscat(tmp, L"…");
    if (textW(dc, f, tmp) <= maxw) lo = mid; else hi = mid - 1;
  }
  if (lo >= cap) lo = cap - 1;
  wcsncpy(out, src, lo); out[lo] = 0; wcscat(out, L"…");
}

/* ------------------------------------------------------------------ JSON */

static const char *findKey(const char *doc, const char *key)
{
  char pat[80];
  const char *p;
  sprintf(pat, "\"%s\"", key);
  p = strstr(doc, pat);
  if (!p) return NULL;
  p += strlen(pat);
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  if (*p != ':') return NULL;
  p++;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  return p;
}

static int jsonStrAt(const char *p, WCHAR *out, int cap)
{
  char buf[2048];
  int bi = 0, i;
  out[0] = 0;
  if (!p || *p != '"') return 0;
  p++;
  while (*p && *p != '"' && bi < 2000) {
    if (*p == '\\' && p[1]) {
      char e = p[1];
      if (e == 'n') buf[bi++] = '\n';
      else if (e == 'r') buf[bi++] = '\r';
      else if (e == 't') buf[bi++] = '\t';
      else if (e == 'u' && p[2] && p[3] && p[4] && p[5]) {
        int v = 0, k;
        for (k = 2; k <= 5; k++) {
          char ch = p[k];
          v = v * 16 + (ch >= '0' && ch <= '9' ? ch - '0' :
                        ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 :
                        ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : 0);
        }
        buf[bi++] = (char)(v & 255);
        p += 4;
      } else {
        buf[bi++] = e;
      }
      p += 2;
    } else {
      buf[bi++] = *p++;
    }
  }
  buf[bi] = 0;
  out[0] = 0;
  {
    /* 必须自己补结尾：cbMultiByte 不为 -1 时 MultiByteToWideChar 不会加 \0 */
    int cw = MultiByteToWideChar(CP_UTF8, 0, buf, bi, out, cap - 1);
    if (cw < 0) cw = 0;
    if (cw > cap - 1) cw = cap - 1;
    out[cw] = 0;
  }
  (void)i;
  return 1;
}

static int jsonStr(const char *doc, const char *key, WCHAR *out, int cap)
{
  return jsonStrAt(findKey(doc, key), out, cap);
}

static int jsonNum(const char *doc, const char *key)
{
  const char *p = findKey(doc, key);
  if (!p) return 0;
  return (int)strtol(p, NULL, 10);
}

static int jsonBool(const char *doc, const char *key)
{
  const char *p = findKey(doc, key);
  if (!p) return 0;
  if (!strncmp(p, "true", 4)) return 1;
  if (!strncmp(p, "\"true\"", 6)) return 1;
  if (*p == '1' || !strncmp(p, "\"1\"", 3)) return 1;
  return 0;
}

/* 取数组里的第 idx 个字符串：先截出原始字节，再整体转成宽字符（避免残留串尾） */
static int jsonArrAt(const char *doc, const char *key, int idx, WCHAR *out, int cap)
{
  const char *p = findKey(doc, key);
  int i = 0;
  out[0] = 0;
  if (!p || *p != '[') return 0;
  p++;
  while (*p && *p != ']') {
    const char *s, *e;
    while (*p == ' ' || *p == ',' || *p == '\r' || *p == '\n' || *p == '\t') p++;
    if (*p != '"') break;
    s = p + 1;
    e = s;
    while (*e && *e != '"') { if (*e == '\\' && e[1]) e++; e++; }
    if (i == idx) {
      char tmp[1024];
      int n = (int)(e - s);
      int cw;
      if (n > 1023) n = 1023;
      memcpy(tmp, s, n);
      tmp[n] = 0;
      out[0] = 0;
      cw = MultiByteToWideChar(CP_UTF8, 0, tmp, n, out, cap - 1);
      if (cw < 0) cw = 0;
      if (cw > cap - 1) cw = cap - 1;
      out[cw] = 0;
      return 1;
    }
    i++;
    if (!*e) break;
    p = e + 1;
  }
  return 0;
}

static void joinArray(const char *doc, const char *key, WCHAR *out, int cap, int maxItems)
{
  WCHAR item[400];
  int i, len = 0;
  out[0] = 0;
  for (i = 0; i < maxItems; i++) {
    if (!jsonArrAt(doc, key, i, item, 400)) break;
    if (item[0]) {
      if (len > 0) { wcsncat(out, L"\n", cap - len); len++; }
      wcsncat(out, item, cap - len - 1);
      len = (int)wcslen(out);
    }
  }
}

/* 从缓存的 json 原文里取第 idx 条 how */
static int glassStep(int idx, WCHAR *out, int cap)
{
  return jsonArrAt(g_glassDoc, "how", idx, out, cap);
}

static char *readFileUtf8(const WCHAR *path, long *sizeOut){
  HANDLE f;
  DWORD size, got = 0;
  char *buf;
  f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE) return NULL;
  size = GetFileSize(f, NULL);
  if (size == INVALID_FILE_SIZE || size > 1024 * 512) { CloseHandle(f); return NULL; }
  buf = (char *)malloc(size + 1);
  if (!buf) { CloseHandle(f); return NULL; }
  ReadFile(f, buf, size, &got, NULL);
  CloseHandle(f);
  buf[got] = 0;
  if (sizeOut) *sizeOut = (long)got;
  return buf;
}

static void loadData(void)
{
  long size = 0;
  char *raw = readFileUtf8(gJsonPath, &size);
  if (!raw) { gd.hasData = 0; gd.on = 0; return; }
  ZeroMemory(&gd, sizeof(gd));
  gd.hasData = 1;
  gd.on = jsonBool(raw, "on");
  gd.ok = jsonBool(raw, "ok");
  gd.line = jsonNum(raw, "line");
  gd.ts = 0;
  jsonStr(raw, "kind", gd.kind, 16);
  jsonStr(raw, "level", gd.level, 24);
  jsonStr(raw, "title", gd.title, 160);
  jsonStr(raw, "cause", gd.cause, 520);
  jsonStr(raw, "where", gd.where, 300);
  jsonStr(raw, "code", gd.code, 1200);
  jsonStr(raw, "tip", gd.tip, 300);
  jsonStr(raw, "bad", gd.bad, 300);
  jsonStr(raw, "good", gd.good, 300);
  jsonStr(raw, "name", gd.name, 160);
  joinArray(raw, "how", gd.how, 600, 3);
  joinArray(raw, "tips", gd.tips, 300, 3);
  gd.ts = GetTickCount();
  free(raw);
}

/* ------------------------------------------------------------------ 窗口 */

static void present(Canvas *c, HWND h, int x, int y)
{
  HDC screen = GetDC(NULL);
  POINT dst, src;
  SIZE sz;
  BLENDFUNCTION bf;
  dst.x = x; dst.y = y; src.x = 0; src.y = 0;
  sz.cx = c->w; sz.cy = c->h;
  bf.BlendOp = AC_SRC_OVER;
  bf.BlendFlags = 0;
  bf.SourceConstantAlpha = 255;
  bf.AlphaFormat = AC_SRC_ALPHA;
  SetWindowPos(h, HWND_TOPMOST, x, y, c->w, c->h, SWP_NOACTIVATE | SWP_NOOWNERZORDER);
  UpdateLayeredWindow(h, screen, &dst, &sz, c->hdc, &src, 0, &bf, ULW_ALPHA);
  ShowWindow(h, SW_SHOWNA);
  ReleaseDC(NULL, screen);
}

static void hideWindow(HWND h)
{
  if (h) ShowWindow(h, SW_HIDE);
}

static LRESULT CALLBACK GlassProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg) {
  case WM_MOUSEACTIVATE:
    return MA_NOACTIVATE;
  case WM_LBUTTONDOWN:
    if (h == g_hCard) {
      POINT pt;
      int headH = g_cardHeadH > 0 ? g_cardHeadH : S(24);
      pt.x = (short)LOWORD(lp); pt.y = (short)HIWORD(lp);
      if (pt.y <= headH) {
        GetWindowRect(h, &g_dragRect);
        GetCursorPos(&pt);
        g_dragStartX = pt.x; g_dragStartY = pt.y;
        g_dragging = 0;
        SetCapture(h);
      }
    }
    return 0;
  case WM_MOUSEMOVE:
    if (h == g_hCard && GetCapture() == h) {
      POINT pt;
      GetCursorPos(&pt);
      if (abs(pt.x - g_dragStartX) > 3 || abs(pt.y - g_dragStartY) > 3) g_dragging = 1;
      if (g_dragging) {
        g_userDX = pt.x - g_dragStartX;
        g_userDY = pt.y - g_dragStartY;
        SetWindowPos(h, HWND_TOPMOST, g_dragRect.left + g_userDX, g_dragRect.top + g_userDY,
                     0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
      }
    }
    return 0;
  case WM_LBUTTONUP:
    if (h == g_hCard && GetCapture() == h) {
      POINT pt;
      int headH = g_cardHeadH > 0 ? g_cardHeadH : S(24);
      pt.x = (short)LOWORD(lp); pt.y = (short)HIWORD(lp);
      ReleaseCapture();
      if (!g_dragging && pt.y <= headH) {
        g_collapsed = !g_collapsed;   /* 只有点标题栏/折叠条才切换 */
        g_userDX = 0; g_userDY = 0;   /* 切换后回到自动位置 */
        g_needDraw = 1;
      } else if (g_dragging) {
        g_needDraw = 1;
      }
    }
    return 0;
  case WM_LBUTTONDBLCLK:
    if (h == g_hCard) { g_cardHidden = 1; g_needDraw = 1; }
    return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

static void ensureClass(void)
{
  static int done = 0;
  WNDCLASSEXW wc;
  if (done) return;
  done = 1;
  ZeroMemory(&wc, sizeof(wc));
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = GlassProc;
  wc.hInstance = GetModuleHandleW(NULL);
  wc.hCursor = LoadCursor(NULL, IDC_ARROW);
  wc.lpszClassName = CLASS_GLASS;
  RegisterClassExW(&wc);
}

static HWND mkLayered(int clickThrough)
{
  DWORD ex = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST;
  if (clickThrough) ex |= WS_EX_TRANSPARENT;
  ensureClass();
  return CreateWindowExW(ex, CLASS_GLASS, L"", WS_POPUP, 0, 0, 1, 1,
                         NULL, NULL, GetModuleHandleW(NULL), NULL);
}

/* ------------------------------------------------------------------ 查找窗口 */

static HWND g_best;
static long g_bestArea;

static BOOL CALLBACK enumTop(HWND h, LPARAM lp)
{
  RECT r;
  WCHAR cls[128], title[256];
  (void)lp;
  if (!IsWindowVisible(h)) return TRUE;
  if (!GetWindowRect(h, &r)) return TRUE;
  if ((r.right - r.left) < 800 || (r.bottom - r.top) < 500) return TRUE;
  GetClassNameW(h, cls, 128);
  if (!wcscmp(cls, CLASS_GLASS)) return TRUE;
  GetWindowTextW(h, title, 256);
  if (!wcsstr(title, L"Dev-C++") && wcscmp(cls, L"TMainForm")) {
    /* 顺手记一下小助手浮窗的位置，卡片避让它 */
    if (wcsstr(title, L"DevAI")) { g_panelL = r.left; g_panelT = r.top; g_panelB = r.bottom; }
    return TRUE;
  }
  if (IsIconic(h)) return TRUE;
  if (wcsstr(title, L"DevAI")) return TRUE;
  long area = (long)(r.right - r.left) * (r.bottom - r.top);
  if (area > g_bestArea) { g_bestArea = area; g_best = h; }
  return TRUE;
}

static HWND g_childBest;
static const WCHAR *g_wantClass;
static long g_childArea;

static BOOL CALLBACK enumChild(HWND h, LPARAM lp)
{
  RECT r;
  WCHAR cls[128];
  (void)lp;
  if (!IsWindowVisible(h)) return TRUE;
  if (!GetWindowRect(h, &r)) return TRUE;
  if ((r.right - r.left) < 200 || (r.bottom - r.top) < 60) return TRUE;
  GetClassNameW(h, cls, 128);
  if (wcscmp(cls, g_wantClass)) return TRUE;
  long area = (long)(r.right - r.left) * (r.bottom - r.top);
  if (area > g_childArea) { g_childArea = area; g_childBest = h; }
  return TRUE;
}

static HWND findChildClass(HWND host, const WCHAR *cls, int minW, int minH)
{
  g_childBest = NULL;
  g_childArea = 0;
  g_wantClass = cls;
  EnumChildWindows(host, enumChild, 0);
  (void)minW; (void)minH;
  return g_childBest;
}

/* 下方“编译日志 / 运行结果”面板：优先白名单类名，找不到再放宽到任意子窗口 */
static HWND g_memoBest;
static long g_memoArea;
static int g_memoLoose;

static BOOL CALLBACK enumMemo(HWND h, LPARAM lp)
{
  RECT r;
  WCHAR cls[128];
  int hostW = g_hostR - g_hostL, hostH = g_hostB - g_hostT;
  (void)lp;
  if (!IsWindowVisible(h)) return TRUE;
  if (h == g_hEdit) return TRUE;
  if (!GetWindowRect(h, &r)) return TRUE;
  GetClassNameW(h, cls, 128);
  if (!wcscmp(cls, CLASS_GLASS)) return TRUE;
  if (!g_memoLoose) {
    if (wcscmp(cls, L"TMemo") && wcscmp(cls, L"TSynMemo") && wcscmp(cls, L"TRichEdit") &&
        wcscmp(cls, L"TRichEditVista") && wcscmp(cls, L"TSynEdit"))
      return TRUE;
  }
  if ((r.right - r.left) < hostW / 2) return TRUE;      /* 太窄，不是日志区 */
  if ((r.bottom - r.top) < S(40)) return TRUE;          /* 状态栏之类 */
  if ((r.bottom - r.top) > hostH / 2) return TRUE;      /* 太大，是整块容器 */
  if (r.top < g_edB - S(12)) return TRUE;               /* 必须在编辑区下方 */
  if (r.top > g_hostB - S(40)) return TRUE;
  long area = (long)(r.right - r.left) * (r.bottom - r.top);
  if (area > g_memoArea) { g_memoArea = area; g_memoBest = h; }
  return TRUE;
}

static int locateWindows(void)
{
  RECT r;
  g_best = NULL;
  g_bestArea = 0;
  g_panelL = 0; g_panelT = 0; g_panelB = 0;
  EnumWindows(enumTop, 0);
  if (!g_best) return 0;
  g_hHost = g_best;
  GetWindowRect(g_hHost, &r);
  g_hostL = r.left; g_hostT = r.top; g_hostR = r.right; g_hostB = r.bottom;

  g_hEdit = findChildClass(g_hHost, L"TSynEdit", 300, 100);
  if (!g_hEdit) return 0;
  GetWindowRect(g_hEdit, &r);
  g_edL = r.left; g_edT = r.top; g_edR = r.right; g_edB = r.bottom;

  g_memoBest = NULL;
  g_memoArea = 0;
  g_memoLoose = 0;
  EnumChildWindows(g_hHost, enumMemo, 0);
  if (!g_memoBest) {
    g_memoLoose = 1;
    g_memoArea = 0;
    EnumChildWindows(g_hHost, enumMemo, 0);
    g_memoLoose = 0;
  }
  g_hMemo = g_memoBest;
  if (g_hMemo) {
    GetWindowRect(g_hMemo, &r);
    g_memoL = r.left; g_memoT = r.top; g_memoR = r.right; g_memoB = r.bottom;
  } else if (g_hostB - g_edB > S(90)) {
    /* 找不到日志控件（面板未展开/自绘）：就用编辑区下方那块空白区域 */
    g_memoL = g_edL;
    g_memoR = g_edR;
    g_memoT = g_edB + S(2);
    g_memoB = g_hostB - S(26);
  } else {
    g_memoR = g_memoL = 0;
    g_memoB = g_memoT = 0;
  }
  return 1;
}

static int readDpr(void)
{
  WCHAR path[MAX_PATH];
  char raw[32] = {0};
  HANDLE f;
  wsprintfW(path, L"%s\\state\\dpr.txt", gHome);
  f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE) return 0;
  DWORD got = 0;
  ReadFile(f, raw, 31, &got, NULL);
  CloseHandle(f);
  double v = atof(raw);
  if (v < 0.6 || v > 5) return 0;
  gDpr = v;
  gDpi = (int)(96 * v);
  return 1;
}

/* ------------------------------------------------------------------ 行定位 */

static int g_capW, g_capH;
static Canvas g_cap;
static int g_capReady;

static void captureEditor(void)
{
  int w = g_edR - g_edL, h = g_edB - g_edT;
  HDC screen;
  if (w < 10 || h < 10) return;
  if (!g_capReady || g_capW != w || g_capH != h) {
    if (g_capReady) cvFree(&g_cap);
    if (!cvCreate(&g_cap, w, h)) { g_capReady = 0; return; }
    g_capW = w; g_capH = h; g_capReady = 1;
  }
  if (g_hBand) ShowWindow(g_hBand, SW_HIDE);
  if (g_hCard) ShowWindow(g_hCard, SW_HIDE);
  Sleep(60);
  screen = GetDC(NULL);
  BitBlt(g_cap.hdc, 0, 0, w, h, screen, g_edL, g_edT, SRCCOPY);
  ReleaseDC(NULL, screen);
}

/* 通过扫描像素找到“文字行”的起点与行距 */
static void measureLines(void)
{
  int w = g_capW, h = g_capH, x, y, i, k, sw, x0;
  int bgR = 255, bgG = 255, bgB = 255;
  int starts[80], ends[80], count = 0;
  int thr, minH;
  if (!g_capReady) { g_pitch = S(22); g_firstTextY = S(6); g_inkRight = 0; g_lastCodeY = 0; return; }
  sw = w / 2;
  if (sw < 300) sw = w;
  x0 = S(70);
  if (x0 > sw - 60) x0 = 0;

  /* 背景色：取出现次数最多的颜色，避免取到滚动条或别的窗口 */
  {
    static int hist[4096];
    int best = 0, bestKey = 0;
    for (i = 0; i < 4096; i++) hist[i] = 0;
    for (y = 0; y < h; y += 5)
      for (x = 0; x < sw; x += 7) {
        unsigned int v = g_cap.px[(size_t)y * w + x];
        int key = ((v >> 20) & 15) << 8 | ((v >> 12) & 15) << 4 | ((v >> 4) & 15);
        hist[key]++;
      }
    for (i = 0; i < 4096; i++) if (hist[i] > best) { best = hist[i]; bestKey = i; }
    bgR = ((bestKey >> 8) & 15) * 17;
    bgG = ((bestKey >> 4) & 15) * 17;
    bgB = (bestKey & 15) * 17;
  }
  thr = (sw - x0) / 120;
  if (thr < 3) thr = 3;
  minH = S(6);
  if (minH < 5) minH = 5;

  /* 逐行统计“文字像素”数量：既用来切行，也用来做行距自相关 */
  static int prof[2048];
  int hi = h;
  if (hi > 2048) hi = 2048;
  for (i = 0; i < 2048; i++) prof[i] = 0;

  int inBand = 0;
  int inkRight = 0;   /* 右半幅里最靠右的文字像素，用来找“空白列”放卡片 */
  int lastInk = 0;    /* 最后一小片有字的行，卡片可以放到它下面 */
  for (y = 0; y < h && count < 80; y++) {
    int cnt = 0;
    for (x = x0; x < sw; x += 2) {
      unsigned int v = g_cap.px[(size_t)y * w + x];
      int r = (v >> 16) & 255, g = (v >> 8) & 255, b = v & 255;
      int d = abs(r - bgR) + abs(g - bgG) + abs(b - bgB);
      if (d > 240) cnt++;
    }
    if (y < hi) prof[y] = cnt;
    /* 右半幅粗扫一遍：这一行有没有字（有就精确找最右一个） */
    {
      int rx, hit = 0;
      for (rx = sw + 2; rx < w - S(40); rx += 6) {
        unsigned int v = g_cap.px[(size_t)y * w + rx];
        int r = (v >> 16) & 255, g = (v >> 8) & 255, b = v & 255;
        if (abs(r - bgR) + abs(g - bgG) + abs(b - bgB) > 240) { hit = 1; break; }
      }
      if (hit) {
        for (rx = w - S(40) - 1; rx >= sw + 2; rx--) {
          unsigned int v = g_cap.px[(size_t)y * w + rx];
          int r = (v >> 16) & 255, g = (v >> 8) & 255, b = v & 255;
          if (abs(r - bgR) + abs(g - bgG) + abs(b - bgB) > 240) {
            if (rx > inkRight) inkRight = rx;
            break;
          }
        }
      }
    }
    if (cnt > thr) {
      if (y > lastInk) lastInk = y;
      if (!inBand) { starts[count] = y; ends[count] = y; count++; inBand = 1; }
      else ends[count - 1] = y;
    } else {
      inBand = 0;
    }
  }

  /* 合并相邻很近的碎块（抗锯齿会把一行拆成几段） */
  {
    int m = 0;
    for (i = 0; i < count; i++) {
      if (m > 0 && starts[i] - ends[m - 1] <= S(4)) {
        if (ends[i] > ends[m - 1]) ends[m - 1] = ends[i];
      } else {
        starts[m] = starts[i]; ends[m] = ends[i]; m++;
      }
    }
    count = m;
  }

  /* 去掉太矮的横线（边框、分隔线），只留文字行 */
  for (i = 0, k = 0; i < count; i++) {
    if (ends[i] - starts[i] + 1 >= minH) { starts[k] = starts[i]; ends[k] = ends[i]; k++; }
  }
  count = k;
  if (count == 0) { g_pitch = S(22); g_firstTextY = S(6); g_inkRight = inkRight; g_lastCodeY = 0; return; }

  /* 行距：对行轮廓做自相关，找让“文字行对文字行”最吻合的周期（空白行不影响） */
  {
    int L, pitch = 0;
    long long best = -1;
    for (L = S(12); L <= S(45); L++) {
      long long s = 0;
      for (y = 0; y + L < hi; y++) s += (long long)prof[y] * prof[y + L];
      if (s > best) { best = s; pitch = L; }
    }
    if (pitch <= 0) pitch = S(22);
    g_pitch = pitch;
  }

  /* 首行相位：让每一行的文字顶都落在同一个偏移上（文字最密的那一行对齐） */
  {
    int p, bestP = 0;
    long long bestS = -1;
    for (p = 0; p < g_pitch; p++) {
      long long s = 0;
      for (y = p; y < hi; y += g_pitch) s += prof[y];
      if (s > bestS || (s == bestS && p < bestP)) { bestS = s; bestP = p; }
    }
    g_firstTextY = bestP;
  }
  if (g_firstTextY < 0) g_firstTextY = 0;
  g_inkRight = inkRight;
  g_lastCodeY = (lastInk > 0) ? lastInk + S(4) : 0;
}

static int readTopLine(void)
{
  SCROLLINFO si;
  ZeroMemory(&si, sizeof(si));
  si.cbSize = sizeof(si);
  si.fMask = SIF_ALL;
  if (g_hEdit && GetScrollInfo(g_hEdit, SB_VERT, &si)) {
    if (si.nPos > 0) return si.nPos;
  }
  return 1;
}

/* ------------------------------------------------------------------ 绘制：编辑区玻璃 */

static int g_bandLineY = -1, g_bandLineH = 0;

static void drawRobot(Canvas *c, int x, int y, int targetX, int targetY)
{
  int bw = S(56), bh = S(50);
  int inkR = 34, inkG = 38, inkB = 42;
  int paperR = 255, paperG = 253, paperB = 245;
  /* 天线 */
  cvFill(c, x + bw / 2, y, S(3), S(12), inkR, inkG, inkB, 255);
  cvFill(c, x + bw / 2 - S(4), y - S(6), S(11), S(8), 226, 100, 59, 255);
  /* 身体 */
  cvFill(c, x, y + S(12), bw, bh, paperR, paperG, paperB, 247);
  cvRect(c, x, y + S(12), bw, bh, inkR, inkG, inkB, 255);
  /* 眼睛 */
  cvFill(c, x + S(14), y + S(26), S(8), S(8), inkR, inkG, inkB, 255);
  cvFill(c, x + S(34), y + S(26), S(8), S(8), inkR, inkG, inkB, 255);
  /* 嘴 */
  cvFill(c, x + S(19), y + S(44), S(18), S(3), inkR, inkG, inkB, 255);
  /* 手臂：虚线指向出错行 */
  cvDashedLine(c, x + bw, y + S(30), targetX, targetY, 226, 100, 59, 235, S(8));
}

static void renderBand(void)
{
  Canvas c;
  int w = g_edR - g_edL, h = g_edB - g_edT;
  int x = 0, y = 0;
  int line = gd.line;
  int bandY, bandH;
  WCHAR tag[64];

  if (w < 40 || h < 40) return;
  if (!cvCreate(&c, w, h)) return;

  if (line > 0 && g_bandLineY >= 0) {
    bandY = g_bandLineY;
    bandH = g_bandLineH;
    /* 红色高亮带 */
    cvFill(&c, 0, bandY, w, bandH, 176, 52, 60, 62);
    cvFill(&c, 0, bandY, S(5), bandH, 176, 52, 60, 235);
    /* 右侧标签 */
    wsprintfW(tag, L"第 %d 行", line);
    {
      int tw = textW(c.hdc, g_fTag, tag) + S(12);
      int th = S(20);
      int tx = w - tw - S(8);
      int ty = bandY + (bandH - th) / 2;
      if (tx < S(60)) tx = S(60);
      cvFill(&c, tx, ty, tw, th, 34, 38, 42, 225);
      drawTextAt(&c, c.hdc, tx + S(6), ty + (th - textLineH(c.hdc, g_fTag)) / 2 + S(1),
                 tag, g_fTag, 255, 253, 248, 255);
    }
    /* 小D 站在带子左边，虚线手臂指向这一行 */
    {
      int rx = S(8);
      int ry = bandY - S(54);
      if (ry < S(2)) ry = S(2);
      if (ry + S(96) > h) ry = h - S(98);
      drawRobot(&c, rx, ry, S(48), bandY + bandH / 2);
    }
  } else if (line > 0) {
    /* 出错行不在可视范围：在顶部给个提示 */
    wsprintfW(tag, L"第 %d 行在下面，滚动查看", line);
    {
      int tw = textW(c.hdc, g_fTag, tag) + S(14);
      int th = S(24);
      int tx = w - tw - S(8);
      cvFill(&c, tx, S(8), tw, th, 176, 52, 60, 225);
      drawTextAt(&c, c.hdc, tx + S(7), S(8) + (th - textLineH(c.hdc, g_fTag)) / 2,
                 tag, g_fTag, 255, 244, 244, 255);
    }
  }

  present(&c, g_hBand, g_edL, g_edT);
  cvFree(&c);
}

/* ------------------------------------------------------------------ 绘制：讲解卡 */

/* 折叠时那一行提示的文案：第几行 + 一句短的错误原因 */
static void cardShortTitle(WCHAR *out, int cap)
{
  WCHAR src[200];
  const WCHAR *s = (gd.cause[0] ? gd.cause : gd.title);
  int i, n = 0;
  for (i = 0; s[i] && n < 150; i++) {
    if (s[i] == '\r') continue;
    src[n++] = (s[i] == '\n') ? L' ' : s[i];
  }
  src[n] = 0;
  if (gd.line > 0)
    _snwprintf(out, cap, L"第 %d 行 · ", gd.line);
  else
    _snwprintf(out, cap, L"代码体检 · ");
  out[cap - 1] = 0;
  wcsncat(out, src, cap - 1 - (int)wcslen(out));
  out[cap - 1] = 0;
}

/* 折叠条宽度：跟渲染时算的一模一样，布局才知道放哪 */
static int collapseCardWidth(void)
{
  WCHAR t[220], ell[220];
  int P = S(7);
  cardShortTitle(t, 220);
  copyEllipsis(g_measureDC, g_fBody, t, S(190), ell, 220);
  return P + S(9) + textW(g_measureDC, g_fBody, ell) + S(6) + S(12) + P;
}

/* 折叠态：只有一条很窄的提示，点在它上面才展开 */
static void renderCollapsed(void)
{
  Canvas c;
  WCHAR t[220], ell[220];
  int P = S(7), H = S(24), tw, W;
  int lineH = textLineH(g_measureDC, g_fBody);
  cardShortTitle(t, 220);
  copyEllipsis(g_measureDC, g_fBody, t, S(190), ell, 220);
  tw = textW(g_measureDC, g_fBody, ell);
  W = P + S(9) + tw + S(6) + S(12) + P;
  if (!cvCreate(&c, W, H)) return;
  cvFill(&c, 0, 0, W, H, 255, 253, 248, 226);
  cvRect(&c, 0, 0, W, H, 34, 38, 42, 190);
  cvFill(&c, 0, 0, S(3), H, 198, 58, 58, 255);              /* 左侧红边 */
  cvFill(&c, P, (H - S(5)) / 2, S(5), S(5), 198, 58, 58, 255); /* 小红点 */
  drawTextAt(&c, c.hdc, P + S(9), (H - lineH) / 2 + S(1), ell, g_fBody, 34, 38, 42, 255);
  cvChevron(&c, W - P - S(10), (H - S(8)) / 2, S(4), 0, 105, 111, 118, 255);
  g_cardHeadH = H;
  present(&c, g_hCard, g_cardX, g_cardY);
  cvFree(&c);
}

static void renderCard(void)
{
  Canvas c;
  int W = S(300), P = S(9);
  int capH = (g_edB - g_edT) - S(16);
  int hH = S(30);
  int y, i, n, total;
  WCHAR lines[16][1024];
  int lineHBody, lineHHead, lineHSmall, lineHMono;
  RECT headRect;

  if (!gd.on || gd.ok || !wcscmp(gd.kind, L"busy") || g_cardHidden) { hideWindow(g_hCard); return; }
  if (g_collapsed) { renderCollapsed(); return; }
  if (capH > S(320)) capH = S(320);

  lineHHead = textLineH(g_measureDC, g_fHead);
  lineHBody = textLineH(g_measureDC, g_fBody);
  lineHSmall = textLineH(g_measureDC, g_fSmall);
  lineHMono = textLineH(g_measureDC, g_fMono);

  /* 先测总高度：原因(≤3行) + 改法(≤3行) + 代码(≤4行) + 技巧(≤2行) */
  total = hH + P * 2 + S(16) + 0;
  total += lineHBody * 3 + lineHSmall;
  total += lineHSmall + lineHBody * 3;
  total += lineHMono * 2 + S(8);
  total += lineHSmall + lineHBody * 2;

  if (total > capH) { total = capH; }

  if (!cvCreate(&c, W, total)) return;

  /* 卡片底 */
  cvFill(&c, 0, 0, W, total, 255, 253, 248, 244);
  cvRect(&c, 0, 0, W, total, 34, 38, 42, 255);
  /* 标题栏 */
  cvFill(&c, 1, 1, W - 2, hH - 1, 242, 238, 228, 250);
  cvFill(&c, 1, hH - 1, W - 2, S(2), 34, 38, 42, 255);

  {
    WCHAR head[220];
    int hw;
    if (gd.line > 0)
      wsprintfW(head, L"小D · 第 %d 行", gd.line);
    else
      wcsncpy(head, L"小D · 代码体检", 200);
    head[200] = 0;
    hw = textW(c.hdc, g_fHead, head);
    if (hw > W - S(50)) {
      WCHAR t[220];
      copyEllipsis(c.hdc, g_fHead, head, W - S(56), t, 220);
      drawTextAt(&c, c.hdc, P, (hH - lineHHead) / 2 + S(2), t, g_fHead, 34, 38, 42, 255);
    } else {
      drawTextAt(&c, c.hdc, P, (hH - lineHHead) / 2 + S(2), head, g_fHead, 34, 38, 42, 255);
    }
    cvChevron(&c, W - S(20), (hH - S(8)) / 2, S(4), 1, 105, 111, 118, 255);
  }

  g_cardHeadH = hH;

  y = hH + P;
  /* ① 错误原因 */
  {
    WCHAR label[80];
    wsprintfW(label, L"① 错误原因");
    if (y + lineHSmall < total) {
      drawTextAt(&c, c.hdc, P, y, label, g_fSmall, 176, 52, 60, 255);
      y += lineHSmall + S(2);
      n = wrapLines(c.hdc, g_fBody, gd.cause, W - P * 2, &lines[0][0], 1024, 4, 240);
      for (i = 0; i < n; i++) {
        if (y + lineHBody > total - P) break;
        drawTextAt(&c, c.hdc, P, y, lines[i], g_fBody, 34, 38, 42, 255);
        y += lineHBody;
      }
    }
  }

  y += S(6);
  /* ② 具体改这里 */
  {
    WCHAR label[120];
    WCHAR wline[700];
    wsprintfW(label, L"② 具体改这里");
    if (y + lineHSmall < total) {
      drawTextAt(&c, c.hdc, P, y, label, g_fSmall, 44, 122, 111, 255);
      y += lineHSmall + S(2);
    }
    wline[0] = 0;
    if (gd.where[0]) { wcsncpy(wline, gd.where, 600); wline[600] = 0; }
    if (wline[0] && y + lineHBody <= total - P) {
      copyEllipsis(c.hdc, g_fBody, wline, W - P * 2, lines[0], 1024);
      drawTextAt(&c, c.hdc, P, y, lines[0], g_fBody, 34, 38, 42, 255);
      y += lineHBody;
    }
    /* 改法步骤（最多 2 行） */
    {
      WCHAR step[700], tmp[720];
      int k, cnt = 0;
      for (k = 0; k < 3 && cnt < 2; k++) {
        if (!glassStep(k, step, 700)) break;
        wcsncpy(tmp, L"· ", 700);
        tmp[700] = 0;
        wcsncat(tmp, step, 710);
        n = wrapLines(c.hdc, g_fBody, tmp, W - P * 2 - S(6), &lines[0][0], 1024, 2, 240);
        for (i = 0; i < n && cnt < 2; i++, cnt++) {
          if (y + lineHBody > total - P) break;
          drawTextAt(&c, c.hdc, P + S(4), y, lines[i], g_fBody, 68, 74, 80, 255);
          y += lineHBody;
        }
      }
    }
    /* 代码块 */
    if (gd.code[0] && y + lineHMono * 2 < total - P) {
      WCHAR codeLines[8][600];
      int cl = 0, start = 0, k, len = (int)wcslen(gd.code);
      int boxH;
      for (k = 0; k <= len && cl < 6; k++) {
        if (k == len || gd.code[k] == '\n') {
          int l = k - start;
          if (l > 599) l = 599;
          wcsncpy(codeLines[cl], gd.code + start, l);
          codeLines[cl][l] = 0;
          cl++;
          start = k + 1;
        }
      }
      boxH = cl * lineHMono + S(8);
      if (y + boxH > total - P) boxH = (total - P - y);
      if (boxH > lineHMono) {
        cvFill(&c, P, y, W - P * 2, boxH, 35, 38, 42, 246);
        for (k = 0; k < cl; k++) {
          int ty = y + S(4) + k * lineHMono;
          WCHAR ell[600];
          if (ty + lineHMono > y + boxH) break;
          copyEllipsis(c.hdc, g_fMono, codeLines[k], W - P * 2 - S(10), ell, 600);
          drawTextAt(&c, c.hdc, P + S(6), ty, ell, g_fMono, 234, 229, 218, 255);
        }
        y += boxH + S(6);
      }
    }
  }

  /* ③ 知识点 · 技巧 */
  if (y + lineHSmall + lineHBody < total - P) {
    WCHAR label[80], tips[400];
    wsprintfW(label, L"③ 知识点 · 技巧");
    drawTextAt(&c, c.hdc, P, y, label, g_fSmall, 47, 95, 143, 255);
    y += lineHSmall + S(2);
    tips[0] = 0;
    if (gd.tips[0]) {
      int k;
      WCHAR item[300];
      for (k = 0; k < 3; k++) {
        if (!jsonArrAt(g_glassDoc, "tips", k, item, 300)) break;
        if (tips[0]) wcsncat(tips, L" · ", 390 - (int)wcslen(tips));
        wcsncat(tips, item, 390 - (int)wcslen(tips));
      }
    }
    if (gd.tip[0]) {
      if (tips[0]) wcsncat(tips, L"  ", 390);
      wcsncat(tips, gd.tip, 390 - (int)wcslen(tips));
    }
    if (!tips[0]) wcsncpy(tips, L"编译报错先看第一条，后面的错误多是它引起的。", 390);
    n = wrapLines(c.hdc, g_fBody, tips, W - P * 2, &lines[0][0], 1024, 2, 240);
    for (i = 0; i < n; i++) {
      if (y + lineHBody > total - P) break;
      drawTextAt(&c, c.hdc, P, y, lines[i], g_fBody, 59, 74, 65, 255);
      y += lineHBody;
    }
  }

  headRect.left = 0; headRect.top = 0; headRect.right = W; headRect.bottom = hH;
  (void)headRect;

  present(&c, g_hCard, g_cardX, g_cardY);
  cvFree(&c);
}

/* ------------------------------------------------------------------ 绘制：下方玻璃条 */

static void renderStrip(void)
{
  Canvas c;
  int w = g_memoR - g_memoL, h = g_memoB - g_memoT;
  int P = S(10), y;
  WCHAR t1[400], t2[400], t3[400];
  int hHead, hBody, x;

  if (w < 100 || h < 40) return;
  if (!cvCreate(&c, w, h)) return;

  cvFill(&c, 0, 0, w, h, 255, 253, 248, 238);
  cvFill(&c, 0, 0, S(6), h, 226, 100, 59, 255);
  cvRect(&c, 0, 0, w, h, 34, 38, 42, 200);

  hHead = textLineH(c.hdc, g_fHead);
  hBody = textLineH(c.hdc, g_fBody);
  x = P + S(6);
  y = P;

  if (!gd.on) { cvFree(&c); return; }

  if (!wcscmp(gd.kind, L"busy")) {
    WCHAR busy[400];
    if (gd.title[0]) wcsncpy(busy, gd.title, 300); else wcsncpy(busy, L"正在编译…", 300);
    busy[300] = 0;
    drawTextAt(&c, c.hdc, x, y, busy, g_fHead, 34, 38, 42, 255);
    y += hHead + S(2);
    drawTextAt(&c, c.hdc, x, y, L"小D 在等编译结果…", g_fBody, 91, 97, 103, 255);
    present(&c, g_hStrip, g_memoL, g_memoT);
    cvFree(&c);
    return;
  }

  /* 结果条：标题 + 原因 + 第一条改法 */
  {
    WCHAR head[500];
    if (gd.ok) {
      wsprintfW(head, L"[通过] %s", gd.title[0] ? gd.title : L"编译通过");
    } else if (gd.line > 0) {
      wsprintfW(head, L"[错误] %s · 第 %d 行", gd.title[0] ? gd.title : L"编译失败", gd.line);
    } else {
      wsprintfW(head, L"[错误] %s", gd.title[0] ? gd.title : L"编译失败");
    }
    head[400] = 0;
    copyEllipsis(c.hdc, g_fHead, head, w - x - P, t1, 400);
    drawTextAt(&c, c.hdc, x, y, t1, g_fHead, gd.ok ? 60 : 176, gd.ok ? 122 : 52, gd.ok ? 74 : 60, 255);
    y += hHead + S(3);
  }
  if (gd.cause[0] && y + hBody <= h - P) {
    copyEllipsis(c.hdc, g_fBody, gd.cause, w - x - P, t2, 400);
    drawTextAt(&c, c.hdc, x, y, t2, g_fBody, 34, 38, 42, 255);
    y += hBody;
  }
  if (!gd.ok && y + hBody <= h - P) {
    WCHAR step[500];
    if (glassStep(0, step, 500) && step[0]) {
      WCHAR full[520];
      wsprintfW(full, L"改：%s", step);
      full[500] = 0;
      copyEllipsis(c.hdc, g_fBody, full, w - x - P, t3, 400);
      drawTextAt(&c, c.hdc, x, y, t3, g_fBody, 44, 122, 111, 255);
    } else if (gd.where[0]) {
      copyEllipsis(c.hdc, g_fBody, gd.where, w - x - P, t3, 400);
      drawTextAt(&c, c.hdc, x, y, t3, g_fBody, 44, 122, 111, 255);
    }
  }

  present(&c, g_hStrip, g_memoL, g_memoT);
  cvFree(&c);
}

/* ------------------------------------------------------------------ 布局 */

static void layout(void)
{
  int edW = g_edR - g_edL, edH = g_edB - g_edT;
  int line = gd.line;
  int ch = 0;
  g_bandLineY = -1; g_bandLineH = 0;

  if (line > 0) {
    int rel = line - g_topLine;
    /* 行距里有一截是行间空隙，往上收一点让色带正好裹住这一行文字 */
    int y = g_firstTextY + rel * g_pitch - S(3);
    if (rel >= 0 && y >= 0 && y + g_pitch <= edH + S(4)) {
      g_bandLineY = y;
      g_bandLineH = g_pitch - S(3);
    }
  }

  /* 卡片位置：优先放在“没字的空白处”，实在没地方才贴着出错行右边 */
  {
    int cw2 = g_collapsed ? collapseCardWidth() : S(300);
    int cardH = g_collapsed ? S(24) : S(300);
    int wantX, wantY;
    int used = 0;

    /* ① 代码最后一行下面的空白地带（左边缩进对齐，最不挡东西） */
    if (g_lastCodeY > 0 && edH - (g_lastCodeY + S(10)) >= cardH + S(4)) {
      wantX = S(50);
      wantY = g_lastCodeY + S(10);
      used = 1;
    }
    /* ② 可见代码右侧的空列 */
    if (!used && g_inkRight > 0 && edW - g_inkRight - S(14) >= cw2 + S(4)) {
      wantX = g_inkRight + S(12);
      wantY = (g_bandLineY >= 0) ? g_bandLineY + g_bandLineH + S(8) : S(10);
      used = 1;
    }
    /* ③ 贴右边缘 */
    if (!used) {
      wantX = edW - cw2 - S(8);
      wantY = (g_bandLineY >= 0) ? g_bandLineY + g_bandLineH + S(8) : S(10);
    }

    /* 别压住小助手浮窗：压到就往左让 */
    if (g_panelL > g_edL && g_panelT < g_edT + wantY + cardH && g_panelB > g_edT + wantY) {
      int lim = (g_panelL - g_edL) - cw2 - S(8);
      if (lim > S(4) && wantX + cw2 > lim) wantX = lim;
    }

    if (wantX + cw2 > edW - S(4)) wantX = edW - cw2 - S(4);
    if (wantX < S(4)) wantX = S(4);
    if (wantY < S(4)) wantY = S(4);
    if (wantY + cardH > edH - S(4)) wantY = edH - cardH - S(4);
    if (wantY < S(4)) wantY = S(4);

    g_cardX = g_edL + wantX + g_userDX;
    g_cardY = g_edT + wantY + g_userDY;
    (void)ch;
  }
}

/* ------------------------------------------------------------------ 主循环 */

static void writeAlive(void)
{
  HANDLE f = CreateFileW(gAlivePath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE) return;
  DWORD w = 0;
  WriteFile(f, "1", 1, &w, NULL);
  CloseHandle(f);
}

static void hideAll(void)
{
  hideWindow(g_hBand);
  hideWindow(g_hStrip);
  hideWindow(g_hCard);
}

static void initFonts(void)
{
  if (g_fHead) return;
  g_fHead = CreateFontW(-S(14), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                        OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
  g_fBody = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                        OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
  g_fSmall = CreateFontW(-S(11), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
  g_fMono = CreateFontW(-S(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                        OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                        FIXED_PITCH | FF_MODERN, L"Consolas");
  g_fTag = CreateFontW(-S(11), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, LPWSTR cmd, int show)
{
  HANDLE mutex;
  WCHAR exePath[MAX_PATH];
  WCHAR *slash;
  FILETIME lastWrite;
  DWORD lastSize = 0;
  int lastTop = -999, lastLine = -999, lastDrawn = 0;
  int lastEdL = -99999, lastEdT = -99999, lastEdR = -99999, lastEdB = -99999;
  MSG msg;

  (void)hi; (void)hp; (void)show;

  mutex = CreateMutexW(NULL, TRUE, L"DevAIGlassOverlaySingle");
  if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

  SetProcessDPIAware();

  GetModuleFileNameW(NULL, exePath, MAX_PATH);
  slash = wcsrchr(exePath, L'\\');
  if (slash) {
    *slash = 0;
    slash = wcsrchr(exePath, L'\\');
    if (slash) *slash = 0;   /* bin 的上级 = HOME */
    wcscpy_s(gHome, MAX_PATH, exePath);
  }
  wsprintfW(gJsonPath, L"%s\\state\\glass.json", gHome);
  wsprintfW(gAlivePath, L"%s\\state\\glass.alive", gHome);
  wsprintfW(gLogPath, L"%s\\state\\glass.log", gHome);
  DeleteFileW(gLogPath);
  glog(L"start home=%s dpr=%.2f", gHome, gDpr);

  readDpr();
  initFonts();
  g_measureDC = CreateCompatibleDC(NULL);

  g_hBand = mkLayered(1);
  g_hStrip = mkLayered(1);
  g_hCard = mkLayered(0);
  lastWrite.dwLowDateTime = 0; lastWrite.dwHighDateTime = 0;

  while (1) {
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }

    /* 读数据（文件有变化才重读） */
    {
      HANDLE f = CreateFileW(gJsonPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
      if (f != INVALID_HANDLE_VALUE) {
        FILETIME wt;
        GetFileTime(f, NULL, NULL, &wt);
        CloseHandle(f);
        if (wt.dwLowDateTime != lastWrite.dwLowDateTime || wt.dwHighDateTime != lastWrite.dwHighDateTime) {
          long sz = 0;
          char *raw = readFileUtf8(gJsonPath, &sz);
          lastWrite = wt;
          lastSize = (DWORD)sz;
          if (raw) {
            if (sz > 8100) sz = 8100;
            memcpy(g_glassDoc, raw, sz);
            g_glassDoc[sz] = 0;
            free(raw);
            loadData();
            g_needDraw = 1;
            g_userDX = g_userDY = 0;
            g_collapsed = 1;   /* 每次新分析默认折叠，只弹一条小提示 */
            g_cardHidden = 0;
            glog(L"json on=%d kind=%s ok=%d line=%d title=%s", gd.on, gd.kind, gd.ok, gd.line, gd.title);
          }
        }
      } else {
        lastWrite.dwLowDateTime = 0; lastWrite.dwHighDateTime = 0;
        gd.on = 0; gd.hasData = 0;
        g_needDraw = 1;
      }
    }

    /* 找 Dev-C++ 窗口 */
    if (!locateWindows()) {
      hideAll();
      writeAlive();
      static int noHostLogged = 0;
      if (!noHostLogged) { glog(L"no Dev-C++ host/editor"); noHostLogged = 1; }
      Sleep(400);
      continue;
    }
    {
      static HWND loggedHost = NULL;
      if (loggedHost != g_hHost) {
        loggedHost = g_hHost;
        glog(L"host=%p edit=%p editorRect=%d,%d,%d,%d", g_hHost, g_hEdit, g_edL, g_edT, g_edR, g_edB);
      }
    }

    readDpr();
    initFonts();

    /* Dev-C++ 不在前台时就收起玻璃；但焦点在自家面板/玻璃上时仍算“在用 Dev-C++” */
    {
      HWND fg = GetForegroundWindow();
      BOOL active = 0;
      if (fg) {
        WCHAR t[256], cls[128];
        active = (fg == g_hHost || GetAncestor(fg, GA_ROOT) == g_hHost);
        if (!active) {
          GetClassNameW(fg, cls, 128);
          if (!wcscmp(cls, CLASS_GLASS)) active = 1;
          else {
            GetWindowTextW(fg, t, 256);
            if (wcsstr(t, L"DevAI")) active = 1;
          }
        }
      }
      if (!active) {
        hideAll();
        writeAlive();
        Sleep(300);
        continue;
      }
    }
    int edL = g_edL, edT = g_edT, edR = g_edR, edB = g_edB;
    int top = readTopLine();
    int changedGeo = (edL != lastEdL || edT != lastEdT || edR != lastEdR || edB != lastEdB || top != lastTop);

    if (!gd.hasData || !gd.on) {
      hideAll();
      writeAlive();
      lastEdL = edL; lastTop = top; lastLine = gd.line;
      Sleep(230);
      continue;
    }

    /* 过期（5 分钟）就收起，避免挡着 */
    if (gd.ts != 0 && (DWORD)(GetTickCount() - gd.ts) > 300000) {
      hideAll();
      writeAlive();
      Sleep(230);
      continue;
    }

    if (changedGeo) {
      captureEditor();
      if (gd.line > 0 && !wcscmp(gd.kind, L"result")) measureLines();
      g_needDraw = 1;
      lastEdL = edL; lastEdT = edT; lastEdR = edR; lastEdB = edB; lastTop = top;
    } else if (g_needDraw) {
      if (g_pitch <= 1) {
        captureEditor();
        measureLines();
      }
    }

    if (g_needDraw || gd.line != lastLine) {
      g_topLine = top;
      layout();
      glog(L"draw kind=%s line=%d bandY=%d bandH=%d pitch=%d firstY=%d top=%d card=%d,%d", gd.kind, gd.line, g_bandLineY, g_bandLineH, g_pitch, g_firstTextY, top, g_cardX, g_cardY);
      if (!wcscmp(gd.kind, L"busy")) {
        hideWindow(g_hBand);
        hideWindow(g_hCard);
      } else {
        if (gd.line > 0) renderBand(); else hideWindow(g_hBand);
        renderCard();
      }
      if (g_hMemo || (g_memoR - g_memoL) > S(100)) renderStrip(); else hideWindow(g_hStrip);
      g_needDraw = 0;
      lastLine = gd.line;
      lastTop = top;
      lastDrawn = 1;
    } else if (lastDrawn) {
      /* 只跟随窗口移动 */
      SetWindowPos(g_hBand, HWND_TOPMOST, edL, edT, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
      SetWindowPos(g_hStrip, HWND_TOPMOST, g_memoL, g_memoT, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
      SetWindowPos(g_hCard, HWND_TOPMOST, g_cardX, g_cardY, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    }

    writeAlive();
    Sleep(200);
  }
  return 0;
}
