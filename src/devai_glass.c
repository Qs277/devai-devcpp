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
static int g_stripX = 0, g_stripY = 0, g_stripW = 0, g_stripH = 0;
static int g_colX = 0;        /* 右侧信息栏的起始 x（相对编辑区） */
static int g_colW = 0;        /* 右侧信息栏宽度（放不下时整块挪到代码下面） */
static int g_cardHeadH = 0;   /* 点击标题栏才折叠/展开 */
static int g_inkRight = 0;    /* 可见代码最右侧有字的 x（相对编辑区），卡片放它右边 */
static int g_lastCodeY = 0;   /* 可见代码最后一行的底部（相对编辑区） */
static int g_panelL = 0, g_panelT = 0, g_panelB = 0;  /* 小助手浮窗的位置，卡片别压它 */
static int g_topLineSet = 1;
static char g_glassDoc[8192];   /* 缓存 json 原文 */

static int g_codeX0 = 0;        /* 代码第 0 列的 x（相对编辑区） */
static int g_charW = 0;         /* 编辑区等宽字符宽度 */
static int g_forceMeasure = 0;  /* 要求下一次重新截图测量 */
static int g_targetRowTop = -1; /* 出错行在画面上真实文字范围（避免色带上下漂移） */
static int g_targetRowBot = -1;
static int g_sigLine = -999, g_sigBandY = -999, g_sigBandH = -999;
static int g_sigCardX = -999, g_sigCardY = -999, g_sigCollapsed = -1;
static int g_sigStripX = -999, g_sigStripY = -999;
static int g_bgR = 255, g_bgG = 255, g_bgB = 255;
static char g_editText[65536];  /* 编辑区全文（真实文本，用来定位是哪几行） */
static int g_editTextLen = 0;

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
  int col;
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

/* 返回合法的 UTF-8 前缀长度；-1 表示不是 UTF-8 */
static int utf8ValidLen(const char *s, int n)
{
  int i = 0;
  while (i < n) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { i++; continue; }
    if ((c & 0xE0) == 0xC0) {
      if (c < 0xC2) return -1;
      if (i + 1 >= n || ((unsigned char)s[i + 1] & 0xC0) != 0x80) return i;
      i += 2; continue;
    }
    if ((c & 0xF0) == 0xE0) {
      if (i + 2 >= n) return i;
      if (((unsigned char)s[i + 1] & 0xC0) != 0x80 || ((unsigned char)s[i + 2] & 0xC0) != 0x80) return -1;
      i += 3; continue;
    }
    if ((c & 0xF8) == 0xF0) {
      if (i + 3 >= n) return i;
      if (((unsigned char)s[i + 1] & 0xC0) != 0x80 || ((unsigned char)s[i + 2] & 0xC0) != 0x80 ||
          ((unsigned char)s[i + 3] & 0xC0) != 0x80) return -1;
      i += 4; continue;
    }
    return -1;
  }
  return n;
}

/* 先按 UTF-8 解；解不通（有些工具会写成 GBK）就按系统 ANSI 解，避免中文乱码 */
static void decodeBytes(const char *buf, int n, WCHAR *out, int cap)
{
  int cw = 0;
  if (cap < 2) { if (cap == 1) out[0] = 0; return; }
  out[0] = 0;
  if (n <= 0) return;
  {
    int vn = utf8ValidLen(buf, n);
    /* 结尾被截断几个字节也当 UTF-8，丢掉尾巴即可 */
    if (vn == n || (vn > 0 && vn >= n - 3)) {
      if (vn < n) n = vn;
      cw = MultiByteToWideChar(CP_UTF8, 0, buf, n, out, cap - 1);
    }
  }
  if (cw <= 0)
    cw = MultiByteToWideChar(CP_ACP, 0, buf, n, out, cap - 1);
  if (cw < 0) cw = 0;
  if (cw > cap - 1) cw = cap - 1;
  out[cw] = 0;
}

static int jsonStrAt(const char *p, WCHAR *out, int cap)
{
  char buf[2048];
  int bi = 0;
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
        /* \uXXXX 要按 UTF-8 写回字节，直接截成单字节会变乱码 */
        if (v < 0x80) {
          buf[bi++] = (char)v;
        } else if (v < 0x800) {
          buf[bi++] = (char)(0xC0 | (v >> 6));
          buf[bi++] = (char)(0x80 | (v & 0x3F));
        } else {
          buf[bi++] = (char)(0xE0 | (v >> 12));
          buf[bi++] = (char)(0x80 | ((v >> 6) & 0x3F));
          buf[bi++] = (char)(0x80 | (v & 0x3F));
        }
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
  decodeBytes(buf, bi, out, cap);
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
      decodeBytes(tmp, n, out, cap);
      (void)cw;
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
  gd.col = jsonNum(raw, "col");
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
        g_cardX = g_dragRect.left + g_userDX;
        g_cardY = g_dragRect.top + g_userDY;
        SetWindowPos(h, HWND_TOPMOST, g_cardX, g_cardY,
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
      g_dragging = 0;
    }
    return 0;
  case WM_LBUTTONDBLCLK:
    if (h == g_hCard) { g_cardHidden = 1; g_needDraw = 1; }
    return 0;
  case WM_CAPTURECHANGED:
    g_dragging = 0;   /* 拖到一半失去捕获时不要卡在“拖动中” */
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
  GetClassNameW(h, cls, 128);
  if (!wcscmp(cls, CLASS_GLASS)) return TRUE;
  GetWindowTextW(h, title, 256);
  if (wcsstr(title, L"DevAI")) {
    /* 小助手浮窗位置单独记下，卡片要避开它（它通常比主窗窄，不能受下面的尺寸过滤影响） */
    g_panelL = r.left; g_panelT = r.top; g_panelB = r.bottom;
    return TRUE;
  }
  if ((r.right - r.left) < 800 || (r.bottom - r.top) < 500) return TRUE;
  if (!wcsstr(title, L"Dev-C++") && wcscmp(cls, L"TMainForm")) return TRUE;
  if (IsIconic(h)) return TRUE;
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
  static DWORD last = 0;
  static int cached = 0;
  WCHAR path[MAX_PATH];
  char raw[32] = {0};
  HANDLE f;
  /* dpr 基本不会变，3 秒读一次足够，没必要每轮都开文件 */
  if (cached && GetTickCount() - last < 3000) return 1;
  last = GetTickCount();
  cached = 1;
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

/* 这一行（相对编辑区 y 起止）里，代码文字的 x 范围 */
static int textInkRight(void);

/* 行号栏分隔线这类“每一行都有墨”的竖线，不能算成代码 */
static unsigned char g_vertCol[8192];

static void rowInkRange(int y, int hh, int *xl, int *xr, int *xlim)
{
  int x, yy, minx = -1, maxx = -1;
  int w = g_capW, lim, x0;
  *xl = -1; *xr = -1;
  if (xlim) *xlim = 0;
  if (!g_capReady) return;
  lim = w;
  x0 = S(50);
  /* 已经知道代码第 0 列在哪，就从它左边一点开始扫，行号栏的墨迹不会被算成代码 */
  if (g_codeX0 > S(30)) x0 = g_codeX0 - S(4);
  if (x0 < S(6)) x0 = S(6);
  {   /* 代码本身最远到哪：别把右侧面板/提示条的墨迹当成代码 */
    int ti = textInkRight();
    if (ti > 0 && ti + S(30) < lim) lim = ti + S(30);
  }
  if (g_panelL > g_edL) {
    int pl = g_panelL - g_edL - S(4);
    if (pl > x0 && pl < lim) lim = pl;
  }
  if (xlim) *xlim = lim;
  for (yy = y; yy < y + hh; yy++) {
    if (yy < 0 || yy >= g_capH) continue;
    for (x = x0; x < lim; x++) {
      if (x < 8192 && g_vertCol[x]) continue;
      unsigned int v = g_cap.px[(size_t)yy * w + x];
      int r = (v >> 16) & 255, g = (v >> 8) & 255, b = v & 255;
      if (abs(r - g_bgR) + abs(g - g_bgG) + abs(b - g_bgB) > 150) {
        if (minx < 0 || x < minx) minx = x;
        if (x > maxx) maxx = x;
      }
    }
  }
  *xl = minx; *xr = maxx;
}

/* 读编辑区全文。统一转成 UTF-8 存在 g_editText 里，行号才能和画面完全对上 */
static int readEditorText(void)
{
  WCHAR title[600], *p;
  static WCHAR wbuf[32768];
  int n = 0;
  /* 首选：从 Dev-C++ 标题栏拿文件路径，直接读磁盘上的源码。
     编译器报错本来就是针对已保存的文件，这样拿到的行号和画面完全对得上。 */
  if (g_hHost && GetWindowTextW(g_hHost, title, 600) > 0) {
    p = wcsstr(title, L" - Dev-C++");
    if (p && p > title) {
      *p = 0;
      while (p > title && (p[-1] == L' ' || p[-1] == L'*')) { p--; *p = 0; }
      if (wcslen(title) > 4 && title[1] == L':') {   /* 必须是绝对路径，否则交给控件文本 */
        HANDLE f = CreateFileW(title, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
          DWORD rd = 0, sz = GetFileSize(f, NULL);
          if (sz != INVALID_FILE_SIZE && sz > 0) {
            static char raw[65535];
            if (sz > (DWORD)(sizeof(raw) - 1)) sz = (DWORD)(sizeof(raw) - 1);
            if (ReadFile(f, raw, sz, &rd, NULL) && rd > 0) {
              decodeBytes(raw, (int)rd, wbuf, 32768);
              n = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, g_editText,
                                      (int)sizeof(g_editText) - 1, NULL, NULL);
              if (n > 0) n--; else n = 0;
            }
          }
          CloseHandle(f);
        }
      }
    }
  }
  /* 次选：直接问编辑控件。先按 Unicode 取（USER32 会自己做编码转换） */
  if (n <= 0 && g_hEdit) {
    int got = 0;
    int wl = (int)SendMessageW(g_hEdit, WM_GETTEXTLENGTH, 0, 0);
    if (wl > 0 && wl < 32000) {
      wbuf[0] = 0;
      got = (int)SendMessageW(g_hEdit, WM_GETTEXT, (WPARAM)32700, (LPARAM)wbuf);
      if (got > 0) {
        if (got > 32700) got = 32700;
        wbuf[got] = 0;
        n = WideCharToMultiByte(CP_UTF8, 0, wbuf, got, g_editText,
                                (int)sizeof(g_editText) - 1, NULL, NULL);
        if (n <= 0) got = 0;
      }
    }
    if (!got) {
      int an = (int)SendMessageA(g_hEdit, WM_GETTEXT, (WPARAM)(sizeof(g_editText) - 1),
                                 (LPARAM)g_editText);
      if (an > 0) {
        decodeBytes(g_editText, an, wbuf, 32768);
        n = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, g_editText,
                                (int)sizeof(g_editText) - 1, NULL, NULL);
        if (n > 0) n--; else n = 0;
      }
    }
  }
  if (n <= 0) {
    if (g_editTextLen > 0) return 1;   /* 偶尔取不到就别把上一份好文本清掉 */
    g_editText[0] = 0;
    g_editTextLen = 0;
    return 0;
  }
  if (n > (int)sizeof(g_editText) - 1) n = (int)sizeof(g_editText) - 1;
  g_editText[n] = 0;
  g_editTextLen = n;
  return 1;
}

static int textLineCount(int *off, int cap)
{
  int i, n = 0;
  off[0] = 0;
  for (i = 0; i < g_editTextLen && n < cap - 1; i++) {
    if (g_editText[i] == '\n') { n++; off[n] = i + 1; }
  }
  return n + 1;
}

/* 一行的可视列数（tab 按 4 展开、GBK 双字节算 2 列），顺便给出缩进列数 */
static int lineVisual(int s, int e, int *indent)
{
  int i, col = 0, last = 0, ind = -1;
  for (i = s; i < e; i++) {
    unsigned char ch = (unsigned char)g_editText[i];
    int w = 1;
    if (ch == '\r' || ch == '\n') continue;
    if (ch == '\t') w = 4 - (col % 4);
    else if (ch >= 0x81 && ch < 0xFF && i + 1 < e) { w = 2; i++; }
    if (ind < 0 && ch != ' ' && ch != '\t' && ch != '\r') ind = col;
    col += w;
    if (ch != ' ' && ch != '\t' && ch != '\r') last = col;
  }
  if (indent) *indent = (ind < 0) ? col : ind;
  return last;
}

/* 纵向滚动条读出来的行信息（1 起的第一可见行 / 总行数 / 一屏行数） */
static int g_scrollTop = 0, g_scrollLines = 0, g_scrollPage = 0;

/* 用编辑器的真实文本反推“当前第一可见行”：把每行字宽和画面上量到的行尾对齐 */
static void detectTopLine(int *starts, int *ends, int count)
{
  static int off[4096];
  int lc, i, t, rn = 0, bestT = 0, bestX0 = 0, bestCw = 0, scrollOK = 0;
  double bestCost = 1e18;
  int ks[60], inkS[60], inkE[60], rowY[60], rowTop[60], rowBot[60], rowLim[60];
  int pitch = g_pitch > 0 ? g_pitch : S(22);
  int firstY = g_firstTextY;
  g_targetRowTop = g_targetRowBot = -1;
  if (!readEditorText()) return;
  lc = textLineCount(off, 4096);
  /* 滚动条可信（总行数对得上）时，它给的第一可见行最准；
     像素拟合在代码高度重复的文件里容易整体错几行。 */
  scrollOK = (g_scrollTop > 0 && g_scrollPage > 0 && g_scrollLines > 0 &&
              abs(g_scrollLines - lc) <= 2);
  if (scrollOK) g_topLine = g_scrollTop;
  for (i = 0; i < count && rn < 60; i++) {
    int a, b, lm = 0, k2;
    rowInkRange(starts[i], ends[i] - starts[i] + 1, &a, &b, &lm);
    if (a < 0 || b <= a) continue;
    k2 = (starts[i] - firstY + pitch / 2) / pitch;
    if (k2 < 0 || k2 > 200) continue;
    ks[rn] = k2; inkS[rn] = a; inkE[rn] = b; rowY[rn] = starts[i];
    rowTop[rn] = starts[i]; rowBot[rn] = ends[i]; rowLim[rn] = lm; rn++;
  }
  if (rn < 3) return;
  /* 只用“行尾”拟合：行号栏在左边，行尾的墨迹完全不受它影响。
     行尾 x ≈ 代码原点 + 字符宽 × 可视列数，两两斜率取中位数抗噪。 */
  for (t = 1; t <= lc; t++) {
    static double ev[64], lv[64], sl[2048], xv[64];
    int n = 0, m = 0, a, b, cmp = 0, vlen, vi;
    int sOff, eOff, li;
    double cw, x0, cost = 0;
    for (i = 0; i < rn; i++) {
      li = (t - 1) + ks[i];
      if (li < 0 || li >= lc) continue;
      sOff = off[li];
      eOff = (li + 1 < lc) ? off[li + 1] : g_editTextLen;
      vlen = lineVisual(sOff, eOff, &vi);
      if (vlen < 6) continue;                                 /* 太短的行右端不准 */
      if (rowLim[i] > 0 && inkE[i] >= rowLim[i] - S(3)) continue;  /* 行尾被裁掉 */
      ev[n] = inkE[i]; lv[n] = vlen; n++;
    }
    if (n < 3) continue;
    for (a = 0; a < n; a++)
      for (b = a + 1; b < n; b++) {
        double dv = lv[a] - lv[b];
        if (fabs(dv) < 6) continue;
        if (m < 2048) sl[m++] = (ev[a] - ev[b]) / dv;
      }
    if (m < 4) continue;
    for (a = 1; a < m; a++) {
      double v = sl[a];
      for (b = a - 1; b >= 0 && sl[b] > v; b--) sl[b + 1] = sl[b];
      sl[b + 1] = v;
    }
    cw = sl[m / 2];
    if (cw < S(6) || cw > S(30)) continue;
    /* 代码原点用“行首”算：行首墨迹 = 原点 + 字符宽 × 缩进列数。
       行号栏分隔线已经在 rowInkRange 里排除了，所以行首很干净。 */
    for (i = 0, m = 0; i < rn; i++) {
      li = (t - 1) + ks[i];
      if (li < 0 || li >= lc) continue;
      sOff = off[li];
      eOff = (li + 1 < lc) ? off[li + 1] : g_editTextLen;
      vlen = lineVisual(sOff, eOff, &vi);
      if (vlen <= 0) continue;
      xv[m++] = inkS[i] - cw * vi;
    }
    if (m < 2) continue;
    for (a = 1; a < m; a++) {
      double v = xv[a];
      for (b = a - 1; b >= 0 && xv[b] > v; b--) xv[b + 1] = xv[b];
      xv[b + 1] = v;
    }
    x0 = xv[m / 2];
    if (x0 < 0 || x0 > S(200)) continue;
    for (i = 0; i < rn; i++) {
      li = (t - 1) + ks[i];
      if (li < 0 || li >= lc) continue;
      sOff = off[li];
      eOff = (li + 1 < lc) ? off[li + 1] : g_editTextLen;
      vlen = lineVisual(sOff, eOff, &vi);
      if (vlen <= 0) continue;
      cost += fabs((double)inkS[i] - (x0 + cw * vi));
      cost += 0.4 * fabs((double)inkE[i] - (x0 + cw * vlen));
      cmp++;
    }
    if (cmp < 2 || cmp * 3 < rn * 2) continue;
    cost /= cmp;
    if (cost < bestCost) { bestCost = cost; bestT = t; bestX0 = (int)x0; bestCw = (int)cw; }
  }
  if (scrollOK && bestT > 0) bestT = g_scrollTop;
  if (bestT > 0 && bestCw > 0) {
    int ph[60], pn = 0, bestD = 1 << 30, avg;
    long sum = 0;
    g_topLine = bestT;
    g_codeX0 = bestX0;
    g_charW = bestCw;
    /* 相位：让行带正好压在扫描到的文字行上，避免上下漂移 */
    for (i = 0; i < rn; i++) {
      int v = rowY[i] - ks[i] * pitch;
      if (v >= 0 && v < pitch) { ph[pn++] = v; sum += v; }
    }
    if (pn > 0) {
      avg = (int)(sum / pn);
      for (i = 0; i < pn; i++) {
        int d = ph[i] - avg; if (d < 0) d = -d;
        if (d < bestD) { bestD = d; g_firstTextY = ph[i]; }
      }
    }
    /* 出错行如果就在画面里，直接用这一行真实的文字上下沿，色带不会上下漂 */
    if (gd.line > 0) {
      int want = gd.line - bestT;
      for (i = 0; i < rn; i++) {
        if (ks[i] == want && rowTop[i] <= rowBot[i]) {
          g_targetRowTop = rowTop[i];
          g_targetRowBot = rowBot[i];
          break;
        }
      }
    }
  }
}

/* 滚动检测：只认「编辑区里的真实鼠标输入」，不再对比像素，避免误判导致反复重绘 */
static HHOOK g_mouseHook;

static int editorHasPoint(POINT pt)
{
  return g_hEdit && pt.x >= g_edL && pt.x < g_edR && pt.y >= g_edT && pt.y < g_edB;
}

static LRESULT CALLBACK MouseHookProc(int code, WPARAM wp, LPARAM lp)
{
  if (code == HC_ACTION && gd.on) {
    MSLLHOOKSTRUCT *ms = (MSLLHOOKSTRUCT *)lp;
    if (wp == WM_MOUSEWHEEL || wp == WM_MOUSEHWHEEL || wp == WM_LBUTTONDOWN) {
      if (editorHasPoint(ms->pt)) g_forceMeasure = 1;
    }
  }
  return CallNextHookEx(g_mouseHook, code, wp, lp);
}

/* 键盘翻页/移动光标也会滚动，同样只认真实输入 */
static HHOOK g_keyHook;

static LRESULT CALLBACK KeyHookProc(int code, WPARAM wp, LPARAM lp)
{
  if (code == HC_ACTION && wp == WM_KEYDOWN && gd.on) {
    KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)lp;
    if (k->vkCode == VK_UP || k->vkCode == VK_DOWN || k->vkCode == VK_PRIOR ||
        k->vkCode == VK_NEXT || k->vkCode == VK_HOME || k->vkCode == VK_END) {
      HWND fg = GetForegroundWindow();
      if (g_hEdit && fg == GetAncestor(g_hEdit, GA_ROOT)) g_forceMeasure = 1;
    }
  }
  return CallNextHookEx(g_keyHook, code, wp, lp);
}

/* 钩子必须装在一个“一直在泵消息”的线程上。挂在主线程时，主线程每轮
   要枚举窗口/抓屏/读文件还 Sleep，系统派给低层钩子的每个键鼠事件都得
   等它空闲才继续 —— 整台机器的鼠标键盘都跟着卡。这里单独开一条只跑
   GetMessage 的线程。 */
static DWORD WINAPI hookThread(LPVOID unused)
{
  MSG m;
  (void)unused;
  g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, GetModuleHandleW(NULL), 0);
  g_keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyHookProc, GetModuleHandleW(NULL), 0);
  while (GetMessageW(&m, NULL, 0, 0) > 0) {
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
  return 0;
}

/* 编辑区文本指纹：打字/粘贴导致滚动时也能发现 */
static long textSig(void)
{
  long s;
  int i;
  if (!readEditorText()) return -1;
  s = g_editTextLen;
  for (i = 0; i < g_editTextLen; i++) s = (s * 131 + (long)g_editText[i]) % 1000000007L;
  return s;
}

static int g_capFallback = 0;

static int captureEditor(void)
{
  int w = g_edR - g_edL, h = g_edB - g_edT;
  HDC screen;
  if (w < 10 || h < 10) return 0;
  if (!g_capReady || g_capW != w || g_capH != h) {
    if (g_capReady) cvFree(&g_cap);
    if (!cvCreate(&g_cap, w, h)) { g_capReady = 0; return 0; }
    g_capW = w; g_capH = h; g_capReady = 1;
  }
  /* 抓屏前先把自己的窗口藏起来，否则会把自己画的高亮/提示条也抓进去 */
  if (g_hBand && IsWindowVisible(g_hBand)) { ShowWindow(g_hBand, SW_HIDE); g_capFallback = 1; }
  if (g_hCard && IsWindowVisible(g_hCard)) { ShowWindow(g_hCard, SW_HIDE); g_capFallback = 1; }
  if (g_hStrip && IsWindowVisible(g_hStrip)) { ShowWindow(g_hStrip, SW_HIDE); g_capFallback = 1; }
  if (g_capFallback) Sleep(70);
  screen = GetDC(NULL);
  BitBlt(g_cap.hdc, 0, 0, w, h, screen, g_edL, g_edT, SRCCOPY);
  ReleaseDC(NULL, screen);
  return 1;
}

/* 通过扫描像素找到“文字行”的起点与行距 */
static void measureLines(void)
{
  int w = g_capW, h = g_capH, x, y, i, k, sw, x0;
  int bgR = 255, bgG = 255, bgB = 255;
  int starts[80], ends[80], count = 0;
  int thr, minH, limR;
  if (!g_capReady) { g_pitch = S(22); g_firstTextY = S(6); g_inkRight = 0; g_lastCodeY = 0; return; }
  sw = w / 2;
  if (sw < 300) sw = w;
  x0 = S(70);
  if (x0 > sw - 60) x0 = 0;
  limR = w - S(40);
  if (g_panelL > g_edL && g_panelL - g_edL - S(4) < limR) limR = g_panelL - g_edL - S(4);
  if (limR < sw + S(20)) limR = w - S(40);

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
  g_bgR = bgR; g_bgG = bgG; g_bgB = bgB;
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
  int inkRight = 0;   /* 有字的最右 x，用来找“空白列”放提示/卡片 */
  int maxInkX = 0;
  int lastInk = 0;    /* 最后一小片有字的行，卡片可以放到它下面 */
  for (y = 0; y < h && count < 80; y++) {
    int cnt = 0;
    for (x = x0; x < sw; x += 2) {
      unsigned int v = g_cap.px[(size_t)y * w + x];
      int r = (v >> 16) & 255, g = (v >> 8) & 255, b = v & 255;
      int d = abs(r - bgR) + abs(g - bgG) + abs(b - bgB);
      if (d > 240) { cnt++; if (x > maxInkX) maxInkX = x; }
    }
    if (y < hi) prof[y] = cnt;
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
  /* 代码最右墨迹：只统计“文字行”上的列。
     滚动条、边框这种每一行都有的竖线不算文字，避免提示条被顶到最右边。 */
  if (count > 0) {
    int cRight = 0;
    memset(g_vertCol, 0, sizeof(g_vertCol));
    for (x = x0; x < limR; x++) {
      int hitRows = 0;
      for (i = 0; i < count; i++) {
        for (y = starts[i]; y <= ends[i]; y++) {
          unsigned int v = g_cap.px[(size_t)y * w + x];
          int r = (v >> 16) & 255, g2 = (v >> 8) & 255, b = v & 255;
          if (abs(r - bgR) + abs(g2 - bgG) + abs(b - bgB) > 150) { hitRows++; break; }
        }
      }
      /* 每一行都有墨 = 行号栏分隔线/滚动条，不是文字 */
      if (count >= 3 && hitRows * 4 >= count * 3 && x < 8192) g_vertCol[x] = 1;
      if (hitRows >= 2 && hitRows < count && x > cRight) cRight = x;
    }
    if (cRight > 0) inkRight = cRight;
  }
  if (count == 0) {
    g_pitch = S(22); g_firstTextY = S(6); g_inkRight = inkRight; g_lastCodeY = 0;
    if (g_codeX0 <= 0) g_codeX0 = S(56);
    if (g_charW <= 0) g_charW = S(9);
    return;
  }

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
  detectTopLine(starts, ends, count);
  if (g_codeX0 <= 0) g_codeX0 = S(56);
  if (g_charW <= 0) g_charW = S(9);
  g_inkRight = inkRight > 0 ? inkRight : maxInkX;
  g_lastCodeY = (lastInk > 0) ? lastInk + S(4) : 0;
}

static int readTopLine(void)
{
  SCROLLINFO si;
  ZeroMemory(&si, sizeof(si));
  si.cbSize = sizeof(si);
  si.fMask = SIF_ALL;
  if (g_hEdit && GetScrollInfo(g_hEdit, SB_VERT, &si)) {
    /* TSynEdit 的纵向滚动条是按“行”走的：nPos 就是第一可见行（1 起）、
       nMax 是总行数、nPage 是一屏行数。这套数字直接读，比像素拟合准。 */
    g_scrollTop = si.nPos;
    g_scrollLines = si.nMax;
    g_scrollPage = si.nPage;
    if (si.nPos > 0) return si.nPos;
  } else {
    g_scrollTop = 0; g_scrollLines = 0; g_scrollPage = 0;
  }
  return 1;
}

/* ------------------------------------------------------------------ 绘制：编辑区玻璃 */

static int g_bandLineY = -1, g_bandLineH = 0;

static void renderBand(void)
{
  Canvas c;
  int w = g_edR - g_edL, h = g_edB - g_edT;
  int line = gd.line;
  int bandY, bandH;

  if (w < 40 || h < 40) return;
  if (!cvCreate(&c, w, h)) return;

  if (line > 0 && g_bandLineY >= 0) {
    int xl = -1, xr = -1;
    bandY = g_bandLineY;
    bandH = g_bandLineH;
    /* 只标“有代码的那一段”，右边空白不涂 */
    rowInkRange(bandY, bandH, &xl, &xr, NULL);

    /* 行号栏里竖一条小红条：不压代码，又能一眼看到是哪一行 */
    {
      int gx = g_codeX0 > S(16) ? g_codeX0 - S(9) : S(2);
      if (gx < S(2)) gx = S(2);
      cvFill(&c, gx, bandY, S(4), bandH, 198, 58, 58, 210);
    }

    if (xl >= 0 && xr >= xl) {
      int bx = xl - S(5), bw2;
      /* 有缩进的行也别让色带从半中间开始：统一从代码第 0 列起 */
      if (g_codeX0 > 0 && bx > g_codeX0) bx = g_codeX0 - S(5);
      if (bx < S(2)) bx = S(2);
      bw2 = (xr - bx) + S(6);
      if (bw2 < S(10)) bw2 = S(10);
      if (bx + bw2 > w) bw2 = w - bx;
      cvFill(&c, bx, bandY, bw2, bandH, 176, 52, 60, 44);       /* 很淡的底色 */
      cvFill(&c, bx, bandY + bandH - S(2), bw2, S(2), 176, 52, 60, 190); /* 底线 */
    } else {
      /* 空行：只给个很小的记号，不横跨整行 */
      int bx = g_codeX0 > 0 ? g_codeX0 : S(52);
      if (bx < S(2)) bx = S(2);
      cvFill(&c, bx, bandY, S(14), bandH, 176, 52, 60, 55);
      cvFill(&c, bx, bandY + bandH - S(2), S(14), S(2), 176, 52, 60, 190);
      xl = bx;
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

/* 折叠态：只有一条很窄的提示，点在它上面才展开 */
static void renderCollapsed(void)
{
  Canvas c;
  WCHAR t[220], ell[220];
  int P = S(7), H = S(24), tw, W, maxw;
  int lineH = textLineH(g_measureDC, g_fBody);
  cardShortTitle(t, 220);
  maxw = (g_colW > 0 ? g_colW : S(240)) - P * 2 - S(22);
  if (maxw < S(60)) maxw = S(60);
  copyEllipsis(g_measureDC, g_fBody, t, maxw, ell, 220);
  tw = textW(g_measureDC, g_fBody, ell);
  W = P + S(9) + tw + S(6) + S(12) + P;
  if (g_colW > 0 && W > g_colW) W = g_colW;
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
  int W = g_colW > 0 ? g_colW : S(300), P = S(9);
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
  int w = g_stripW, h = g_stripH;
  int P = S(10);
  int y, x, tw, hHead, hBody;
  WCHAR t[520], head[520];

  if (w < 60 || h < 30) return;
  if (!cvCreate(&c, w, h)) return;

  cvFill(&c, 0, 0, w, h, 255, 253, 248, 238);
  cvFill(&c, 0, 0, S(4), h, 226, 100, 59, 255);
  cvRect(&c, 0, 0, w, h, 34, 38, 42, 190);

  if (!gd.on) { cvFree(&c); return; }

  hHead = textLineH(c.hdc, g_fHead);
  hBody = textLineH(c.hdc, g_fBody);

  x = P + S(6);
  tw = w - x - P;
  if (tw < S(60)) tw = S(60);

  if (!wcscmp(gd.kind, L"busy")) {
    if (gd.title[0]) wcsncpy(head, gd.title, 400); else wcsncpy(head, L"正在编译…", 400);
    head[400] = 0;
    copyEllipsis(c.hdc, g_fHead, head, tw, t, 400);
    drawTextAt(&c, c.hdc, x, P, t, g_fHead, 34, 38, 42, 255);
    copyEllipsis(c.hdc, g_fBody, L"等待编译结果…", tw, t, 400);
    drawTextAt(&c, c.hdc, x, P + hHead + S(3), t, g_fBody, 91, 97, 103, 255);
    present(&c, g_hStrip, g_stripX, g_stripY);
    cvFree(&c);
    return;
  }

  /* 第一行：结论 + 第几行 */
  if (gd.ok) {
    wsprintfW(head, L"[通过] %s", gd.title[0] ? gd.title : L"编译通过");
  } else if (gd.line > 0) {
    wsprintfW(head, L"[错误] 第 %d 行 · %s", gd.line,
              gd.title[0] ? gd.title : L"编译失败");
  } else {
    wsprintfW(head, L"[错误] %s", gd.title[0] ? gd.title : L"编译失败");
  }
  head[400] = 0;
  copyEllipsis(c.hdc, g_fHead, head, tw, t, 400);
  drawTextAt(&c, c.hdc, x, P, t, g_fHead,
             gd.ok ? 60 : 176, gd.ok ? 122 : 52, gd.ok ? 74 : 60, 255);

  y = P + hHead + S(3);
  if (y + hBody <= h - P) {
    const WCHAR *cause = gd.cause[0] ? gd.cause : (gd.where[0] ? gd.where : L"");
    copyEllipsis(c.hdc, g_fBody, cause, tw, t, 400);
    drawTextAt(&c, c.hdc, x, y, t, g_fBody, 34, 38, 42, 255);
    y += hBody;
  }
  if (!gd.ok && y + hBody <= h - P) {
    WCHAR step[520];
    if (glassStep(0, step, 400) && step[0]) {
      wsprintfW(head, L"改：%s", step);
      head[400] = 0;
      copyEllipsis(c.hdc, g_fBody, head, tw, t, 400);
    } else if (gd.where[0]) {
      copyEllipsis(c.hdc, g_fBody, gd.where, tw, t, 400);
    } else {
      t[0] = 0;
    }
    if (t[0]) drawTextAt(&c, c.hdc, x, y, t, g_fBody, 44, 122, 111, 255);
  }

  present(&c, g_hStrip, g_stripX, g_stripY);
  cvFree(&c);
}

/* ------------------------------------------------------------------ 布局 */

/* 提示条的高度（内容） */
static int stripHeight(void)
{
  int lh = textLineH(g_measureDC, g_fBody);
  int lhHead = textLineH(g_measureDC, g_fHead);
  int th = S(10) + lhHead + S(3) + lh * 2 + S(10);
  return th;
}

/* 用源码文本估「最长一行的右边界」：右侧面板的墨迹不会被当成代码，
   提示条就能贴着代码右边放，而不是被顶到面板边上 */
static int textInkRight(void)
{
  int i, maxc = 0, cur = 0;
  if (g_editTextLen <= 0 || g_charW <= 0) return 0;
  for (i = 0; i < g_editTextLen; i++) {
    unsigned char ch = (unsigned char)g_editText[i];
    if (ch == '\r') continue;
    if (ch == '\n') { if (cur > maxc) maxc = cur; cur = 0; continue; }
    if (ch == '\t') { cur += 4 - (cur % 4); continue; }
    if (ch < 0x80) { cur += 1; continue; }
    if ((ch & 0xE0) == 0xC0) { cur += 1; continue; }   /* 2 字节：算 1 个宽 */
    if ((ch & 0xF0) == 0xE0) { cur += 2; continue; }   /* 3 字节（汉字）：算 2 个宽 */
    if ((ch & 0xF8) == 0xF0) { cur += 2; continue; }
  }
  if (cur > maxc) maxc = cur;
  if (maxc <= 0) return 0;
  return (g_codeX0 > 0 ? g_codeX0 : S(56)) + maxc * g_charW;
}

static void layout(void)
{
  int edW = g_edR - g_edL, edH = g_edB - g_edT;
  int line = gd.line;
  int anchorY = S(10);
  g_bandLineY = -1; g_bandLineH = 0;

  if (line > 0) {
    int rel = line - g_topLine;
    int y, hh;
    if (g_targetRowTop >= 0 && rel >= 0 && g_targetRowBot >= g_targetRowTop) {
      /* 这一行的文字就在画面上，直接用它的真实上下沿，色带不会飘 */
      int maxH = g_pitch - S(8);   /* 最多只占一行，避免两行墨迹粘连时色带变双倍高 */
      y = g_targetRowTop - S(2);
      hh = (g_targetRowBot - g_targetRowTop) + S(8);
      if (maxH > S(12) && hh > maxH) hh = maxH;
    } else {
      y = g_firstTextY + rel * g_pitch - S(3);
      hh = g_pitch - S(3) + S(6);
    }
    if (rel >= 0 && y >= 0 && y + hh <= edH + S(4)) {
      g_bandLineY = y;
      g_bandLineH = hh;
      anchorY = y;
    }
  }

  /* 右侧信息栏：提示条在上、讲解卡在下。
     右边放不下就整块挪到代码最后一行下面，任何情况下都不压代码。 */
  {
    int limitR = edW - S(8);
    int baseX, stripH, cardH, codeR, inkR, avail, colW;

    if (g_panelL > g_edL) {
      int pl = g_panelL - g_edL - S(10);
      if (pl > S(120) && pl < limitR) limitR = pl;
    }

    /* 代码真实右边界：像素测量和文本估算取大的，宁可靠右一点 */
    codeR = g_inkRight > 0 ? g_inkRight : 0;
    inkR = textInkRight();
    /* 文件里有比屏幕还长的行时，文本估算会出界，这时只信像素测量 */
    if (inkR > limitR - S(40)) inkR = 0;
    if (inkR > codeR) codeR = inkR;
    if (codeR <= 0) codeR = g_codeX0 > 0 ? g_codeX0 + S(160) : S(300);
    if (codeR > limitR - S(30)) codeR = limitR - S(30);

    baseX = codeR + S(20);
    avail = limitR - baseX;

    stripH = stripHeight();
    if (stripH > edH - S(20)) stripH = edH - S(20);
    if (stripH < S(40)) stripH = S(40);

    if (avail >= S(150)) {
      colW = avail > S(310) ? S(310) : avail;
    } else {
      /* 右边太窄：放到最后一行代码下面（那里是空白），横着铺开 */
      baseX = S(8);
      colW = limitR - baseX;
      if (colW > S(310)) colW = S(310);
      if (colW < S(140)) colW = S(140);
      anchorY = (g_lastCodeY > 0) ? g_lastCodeY + S(10) : anchorY;
      if (anchorY + stripH + S(30) > edH - S(6)) anchorY = edH - stripH - S(30);
      if (anchorY < S(6)) anchorY = S(6);
    }
    g_colW = colW;
    g_colX = baseX;

    g_stripW = colW;
    g_stripH = stripH;
    g_stripX = g_edL + baseX;
    g_stripY = g_edT + anchorY;

    cardH = g_collapsed ? S(24) : S(320);
    g_cardX = g_edL + baseX;
    g_cardY = g_stripY + stripH + S(6);
    if (g_cardY + cardH > g_edT + edH - S(4)) g_cardY = g_edT + edH - cardH - S(4);
    if (g_cardY < g_edT + S(4)) g_cardY = g_edT + S(4);
    g_cardX += g_userDX;
    g_cardY += g_userDY;

    if (!g_collapsed) {
      /* 展开后别超出编辑区右边/下边 */
      if (g_cardX + g_colW > g_edL + limitR) g_cardX = g_edL + limitR - g_colW;
      if (g_cardX < g_edL + S(4)) g_cardX = g_edL + S(4);
    }
  }
}

/* ------------------------------------------------------------------ 主循环 */

static void writeAlive(void)
{
  /* 心跳 1 秒一次就够，原来每轮都写，主循环 5 次/秒全是磁盘操作 */
  static DWORD last = 0;
  DWORD now = GetTickCount();
  if (last && now - last < 900) return;
  last = now;
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
  {
    HANDLE th = CreateThread(NULL, 0, hookThread, NULL, 0, NULL);
    if (th) CloseHandle(th);
  }

  g_hBand = mkLayered(1);
  g_hStrip = mkLayered(1);
  g_hCard = mkLayered(0);
  lastWrite.dwLowDateTime = 0; lastWrite.dwHighDateTime = 0;

  while (1) {
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }

    /* 拖动讲解卡时只跟手移动窗口，不测量、不重绘，避免卡顿和回弹 */
    if (g_dragging && g_hCard && GetCapture() == g_hCard) {
      writeAlive();
      Sleep(16);
      continue;
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
            g_forceMeasure = 1;
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
      lastDrawn = 0;
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
      /* Dev-C++ 主窗自己被藏起来（比如程序正在 [Executing]）时绝不能画：
         否则玻璃会浮在别的程序上面 */
      if (fg && g_hHost && IsWindowVisible(g_hHost)) {
        WCHAR t[256], cls[128];
        active = (fg == g_hHost || GetAncestor(fg, GA_ROOT) == g_hHost);
        if (!active && g_hEdit && GetAncestor(g_hEdit, GA_ROOT) == fg) active = 1;
        if (!active) {
          GetClassNameW(fg, cls, 128);
          if (!wcscmp(cls, CLASS_GLASS)) active = 1;
          else {
            GetWindowTextW(fg, t, 256);
            if (wcsstr(t, L"DevAI")) active = 1;
            /* 系统/后台服务的隐藏窗口抢焦点时不当成“切走了”，免得玻璃乱闪 */
            else if (!IsWindowVisible(fg)) active = 1;
          }
        }
      }
      if (!active) {
        hideAll();
        lastDrawn = 0;
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
      lastDrawn = 0;
      writeAlive();
      lastEdL = edL; lastTop = top; lastLine = gd.line;
      Sleep(230);
      continue;
    }

    /* 过期（5 分钟）就收起，避免挡着 */
    if (gd.ts != 0 && (DWORD)(GetTickCount() - gd.ts) > 300000) {
      hideAll();
      lastDrawn = 0;
      writeAlive();
      Sleep(230);
      continue;
    }

    if (changedGeo || g_forceMeasure || (g_needDraw && g_pitch <= 1)) {
      captureEditor();
      measureLines();
      g_forceMeasure = 0;
      if (g_capFallback) { g_needDraw = 1; g_capFallback = 0; }
      if (changedGeo) {
        lastEdL = edL; lastEdT = edT; lastEdR = edR; lastEdB = edB; lastTop = top;
      }
    } else {
      /* 没有输入事件时低频比对文本指纹（打字/粘贴后编辑器可能自己滚动） */
      static DWORD lastProbeTick = 0;
      static long lastSig = -2;
      if (GetTickCount() - lastProbeTick > 700) {
        long sig2;
        lastProbeTick = GetTickCount();
        sig2 = textSig();
        if (sig2 >= 0 && sig2 != lastSig) { lastSig = sig2; g_forceMeasure = 1; }
      }
    }

    layout();

    /* !lastDrawn：窗口被收起过（切走/没找到 Dev-C++）就必须重画，
       否则 SetWindowPos 不会把隐藏窗口显示出来，浮层会一直不出现 */
    if (g_needDraw || !lastDrawn || gd.line != lastLine ||
        g_bandLineY != g_sigBandY || g_bandLineH != g_sigBandH ||
        g_cardX != g_sigCardX || g_cardY != g_sigCardY ||
        g_stripX != g_sigStripX || g_stripY != g_sigStripY ||
        g_collapsed != g_sigCollapsed) {
      if (!wcscmp(gd.kind, L"busy")) {
        hideWindow(g_hBand);
        hideWindow(g_hCard);
      } else {
        if (gd.line > 0) renderBand(); else hideWindow(g_hBand);
        renderCard();
      }
      renderStrip();
      g_needDraw = 0;
      g_sigLine = gd.line;
      g_sigBandY = g_bandLineY;
      g_sigBandH = g_bandLineH;
      g_sigCardX = g_cardX;
      g_sigCardY = g_cardY;
      g_sigStripX = g_stripX;
      g_sigStripY = g_stripY;
      g_sigCollapsed = g_collapsed;
      lastLine = gd.line;
      lastTop = top;
      lastDrawn = 1;
    } else if (lastDrawn) {
      /* 只跟随窗口移动 */
      SetWindowPos(g_hBand, HWND_TOPMOST, edL, edT, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
      SetWindowPos(g_hStrip, HWND_TOPMOST, g_stripX, g_stripY, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
      SetWindowPos(g_hCard, HWND_TOPMOST, g_cardX, g_cardY, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    }

    writeAlive();
    Sleep(200);
  }
  return 0;
}
