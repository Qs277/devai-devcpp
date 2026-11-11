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

#define CLASS_GLASS L"DevAIGlassOverlay"

/* ------------------------------------------------------------------ 全局 */

static WCHAR gHome[MAX_PATH] = L"D:\\devai";
static WCHAR gJsonPath[MAX_PATH];
static WCHAR gAlivePath[MAX_PATH];

static double gDpr = 1.0;
static int gDpi = 144;

static HFONT g_fHead, g_fBody, g_fSmall, g_fMono, g_fTag;

static HWND g_hBand, g_hStrip, g_hCard;
static HWND g_hEdit, g_hHost, g_hMemo, g_hScroll;

static int g_hostL, g_hostT, g_hostR, g_hostB;
static int g_edL, g_edT, g_edR, g_edB;       /* 编辑区屏幕坐标 */
static int g_memoL, g_memoT, g_memoR, g_memoB;

static int g_pitch;        /* 行高（像素） */
static int g_firstTextY;   /* 编辑区第一行文字的 y（相对编辑区，测量当时的滚动位置） */
static int g_baseY = 0;    /* 第 1 行的行顶在“文档坐标”里的 y（与滚动位置无关） */
static int g_topLine = 1;  /* 当前第一可见行 */
static int g_scrollPx = 0; /* 已经滚过的像素 */
/* 抓屏前后滚动条位置没变：这一轮量出来的“第 1 行基准”才可信。
   用户快速滚轮时，抓到的画面和读到的滚动位置可能差一帧，基准会被污染，
   滑回顶部就表现为“差一行”。不稳定的一律不采用。 */
static int g_measScrollStable = 1;
/* 滚动锚点：在某次“整幅测量”时记下（第一可见行, 滚动条位置）这一对。
   之后每一帧都用“滚动了多少”去推第一可见行 —— 相对位移不受滚动条 nPos
   的绝对起点、顶部内边距、行距单位的影响，6.x 按行、7.x 按像素都能精确跟。 */
static int g_ancLine = 0, g_ancPos = 0, g_ancValid = 0;

static int g_collapsed = 0;   /* 讲解内容默认展开，点标题栏才折叠 */
static int g_cardHidden = 0;
static int g_userDX = 0, g_userDY = 0;
static int g_userMoved = 0;   /* 用户拖过：位置固定，不再自动贴代码 */
static int g_placed = 0;      /* 已经按默认位置摆过一次：之后就是普通浮窗，不再跟着代码重算 */
static int g_userW = 0;       /* 用户拉出来的宽度（0 = 默认宽） */
static int g_resizing = 0;    /* 正在拖右边缘调宽度 */
static int g_resizeStartX = 0, g_resizeStartW = 0;
static int g_needDraw = 1;
static int g_cardX = 0, g_cardY = 0;
static int g_stripX = 0, g_stripY = 0, g_stripW = 0, g_stripH = 0;
/* 平滑跟随：layout() 算出来的是“目标位置”，这里记的是当前真正画出去的位置，
   每帧向目标靠近 1/3，看起来是滑动过去而不是瞬移/闪烁。 */
static int g_curCardX = 0, g_curCardY = 0, g_curStripX = 0, g_curStripY = 0;
static int g_curValid = 0;
static int cardPX(void) { return g_curValid ? g_curCardX : g_cardX; }
static int cardPY(void) { return g_curValid ? g_curCardY : g_cardY; }
static int stripPX(void) { return g_curValid ? g_curStripX : g_stripX; }
static int stripPY(void) { return g_curValid ? g_curStripY : g_stripY; }
static int g_colW = 0;        /* 右侧信息栏宽度（放不下时整块挪到代码下面） */
static int g_cardHeadH = 0;   /* 点击标题栏才折叠/展开 */
static int g_inkRight = 0;    /* 可见代码最右侧有字的 x（相对编辑区），卡片放它右边 */
static int g_lastCodeY = 0;   /* 可见代码最后一行的底部（相对编辑区） */
static int g_panelL = 0, g_panelT = 0, g_panelB = 0;  /* 小助手浮窗的位置，卡片别压它 */
static HWND g_hPanel = NULL;   /* 小助手浮窗句柄：位置真重合时，代码浮窗沉到它下面 */
static int g_cardInk = 0;      /* 卡片横向基准：整份代码最长一行的右边界（稳定，不随画面抖） */
static int g_inkSig = -1;      /* 上面那个基准是按哪份代码算的（内容指纹） */
static char g_glassDoc[8192];   /* 缓存 json 原文 */

static int g_codeX0 = 0;        /* 代码第 0 列的 x（相对编辑区） */
static int g_charW = 0;         /* 编辑区等宽字符宽度 */
static int g_forceMeasure = 0;  /* 要求下一次重新截图测量 */
static int g_measureTrust = 0;  /* 本次重测是否允许改动行距（尺寸变化/首次测量才允许） */
static int g_targetRowTop = -1; /* 出错行在画面上真实文字范围（避免色带上下漂移） */
static int g_targetRowBot = -1;
/* Dev-C++ 自己给整行涂的底色（红=错误行、蓝=当前行）。
   这些行在扫描文字行时会被跳过，但正是色带最该压上去的位置。 */
static int g_colRowTop[32], g_colRowBot[32], g_colRowKind[32], g_colRowN = 0;
/* 左侧行号栏扫出来的每一条“数字带”的 y 区间（只用来诊断/兜底）。
   Dev-C++ 每一行都画行号，红/蓝变色行也照画，所以拿它量行距最可靠。 */
#define GUT_MAX 400
static int g_gutTop[GUT_MAX], g_gutBot[GUT_MAX], g_gutN = 0;
static int g_gutOK = 0;
static int g_gutPitch = 0;    /* 行距（物理像素，整数，回归得到） */
static int g_gutPhase = 0;    /* 数字带上沿在一个行距内的相位 */
static int g_gutDigitH = 0;   /* 行号数字的高度中位数 */
static int g_gutFirstTop = 0; /* 第一可见行的数字带上沿（绝对 y，相对编辑区） */
/* Dev-C++ 变色行的绝对行号（和 g_colRowTop/Bot 一一对应） */
static int g_colLine[32];
static int g_colScroll = -1;  /* 变色行是在哪个滚动位置量到的（滚动后就不再用它的 y） */
/* 抓屏时能不能把自己的窗口排除掉（Win10 2004+ 不用再藏窗口，就不会闪） */
static int g_capExclOk = 0;
static int g_sigLine = -999, g_sigBandY = -999, g_sigBandH = -999;
static int g_sigBandVis = -1;   /* 上一帧画面上有没有标行 */
static int g_bandVisible = 0;   /* 当前画面上有没有真的标出错误行 */
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
/* 代码浮窗（讲解卡/提示条）尺寸：和玻璃层同一套 DPI 缩放，不再额外缩小 */
static int CS(int px) { return (int)(px * gDpr + 0.5); }

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

/* 代码浮窗（提示条/讲解卡）该插在谁后面：
   和小助手浮窗有重合时，插到它后面 —— 宁可代码浮窗被压住，也不遮住面板。 */
static HWND insertAfterFor(HWND h, int x, int y, int w, int hh)
{
  RECT pr;
  if (h != g_hCard && h != g_hStrip) return HWND_TOPMOST;
  if (!g_hPanel || !IsWindow(g_hPanel)) return HWND_TOPMOST;
  if (!GetWindowRect(g_hPanel, &pr)) return HWND_TOPMOST;
  if (x < pr.right && x + w > pr.left && y < pr.bottom && y + hh > pr.top)
    return g_hPanel;
  return HWND_TOPMOST;
}

static void present(Canvas *c, HWND h, int x, int y)
{
  HDC screen = GetDC(NULL);
  POINT dst, src;
  SIZE sz;
  BLENDFUNCTION bf;
  RECT cur;
  HWND after;
  int needMove = 1;
  /* 位置和大小都没变就不要再 SetWindowPos：对 topmost 的 layered 窗口
     反复设置窗口位置会强制 DWM 重新合成，表现出来就是一直闪。 */
  if (IsWindowVisible(h) && GetWindowRect(h, &cur) &&
      cur.left == x && cur.top == y &&
      (cur.right - cur.left) == c->w && (cur.bottom - cur.top) == c->h)
    needMove = 0;
  after = insertAfterFor(h, x, y, c->w, c->h);
  dst.x = x; dst.y = y; src.x = 0; src.y = 0;
  sz.cx = c->w; sz.cy = c->h;
  bf.BlendOp = AC_SRC_OVER;
  bf.BlendFlags = 0;
  bf.SourceConstantAlpha = 255;
  bf.AlphaFormat = AC_SRC_ALPHA;
  if (needMove)
    SetWindowPos(h, after, x, y, c->w, c->h, SWP_NOACTIVATE | SWP_NOOWNERZORDER);
  UpdateLayeredWindow(h, screen, &dst, &sz, c->hdc, &src, 0, &bf, ULW_ALPHA);
  ShowWindow(h, SW_SHOWNA);
  ReleaseDC(NULL, screen);
}

/* 只是跟着 Dev-C++ 窗口挪位置：位置没变就什么都不做 */
static void moveIfMoved(HWND h, int x, int y)
{
  RECT cur;
  HWND after;
  int w, hh;
  if (!h || !IsWindowVisible(h)) return;
  if (!GetWindowRect(h, &cur)) return;
  w = cur.right - cur.left;
  hh = cur.bottom - cur.top;
  if (cur.left == x && cur.top == y) return;
  after = insertAfterFor(h, x, y, w, hh);
  SetWindowPos(h, after, x, y, 0, 0,
               SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
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
      RECT rc;
      int headH = g_cardHeadH > 0 ? g_cardHeadH : S(24);
      pt.x = (short)LOWORD(lp); pt.y = (short)HIWORD(lp);
      GetClientRect(h, &rc);
      /* 右边缘 12px：拖拽调整浮窗宽度 */
      if (pt.x >= rc.right - S(12)) {
        POINT cur;
        GetCursorPos(&cur);
        g_resizing = 1;
        g_resizeStartX = cur.x;
        g_resizeStartW = g_colW > 0 ? g_colW : S(310);
        SetCapture(h);
        return 0;
      }
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
    if (h == g_hCard) {
      if (g_resizing && GetCapture() == h) {
        POINT cur;
        int w;
        GetCursorPos(&cur);
        w = g_resizeStartW + (cur.x - g_resizeStartX);
        if (w < S(240)) w = S(240);
        if (w > S(760)) w = S(760);
        g_userW = w; g_userMoved = 1; g_needDraw = 1;
        return 0;
      }
      if (GetCapture() == h) {
        POINT pt;
        GetCursorPos(&pt);
        if (abs(pt.x - g_dragStartX) > 3 || abs(pt.y - g_dragStartY) > 3) g_dragging = 1;
        if (g_dragging) {
          g_userDX = pt.x - g_dragStartX;
          g_userDY = pt.y - g_dragStartY;
          g_cardX = g_dragRect.left + g_userDX;
          g_cardY = g_dragRect.top + g_userDY;
          g_userMoved = 1;
          SetWindowPos(h, HWND_TOPMOST, g_cardX, g_cardY,
                       0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
          /* 提示条跟着浮窗一起走 */
          if (g_hStrip) {
            g_stripX = g_cardX;
            g_stripY = g_cardY - g_stripH - S(6);
            SetWindowPos(g_hStrip, HWND_TOPMOST, g_stripX, g_stripY,
                         0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
          }
        }
      }
    }
    return 0;
  case WM_LBUTTONUP:
    if (h == g_hCard && GetCapture() == h) {
      POINT pt;
      int headH = g_cardHeadH > 0 ? g_cardHeadH : S(24);
      pt.x = (short)LOWORD(lp); pt.y = (short)HIWORD(lp);
      ReleaseCapture();
      if (g_resizing) {
        g_resizing = 0;
        g_needDraw = 1;
        return 0;
      }
      if (!g_dragging && pt.y <= headH) {
        g_collapsed = !g_collapsed;   /* 只有点标题栏/折叠条才切换 */
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

/* 把自己的窗口从屏幕截图里排除（WDA_EXCLUDEFROMCAPTURE，Win10 2004+）。
   有了它，抓屏测量时就不用先把浮层藏起来再显示 —— 那个“藏一下”就是
   用户看到的每 200ms 闪一下的根源。返回 1 = 系统支持。 */
static int excludeFromCapture(HWND h)
{
  typedef BOOL (WINAPI *PFN_SetWDA)(HWND, DWORD);
  static PFN_SetWDA pfn = NULL;
  static int probed = 0;
  if (!h) return 0;
  if (!probed) {
    HMODULE m = LoadLibraryW(L"user32.dll");
    probed = 1;
    if (m) pfn = (PFN_SetWDA)(void *)GetProcAddress(m, "SetWindowDisplayAffinity");
  }
  if (!pfn) return 0;
#ifndef CAP_EXCLUDE_MODE
#define CAP_EXCLUDE_MODE 0x00000011   /* WDA_EXCLUDEFROMCAPTURE */
#endif
  return pfn(h, CAP_EXCLUDE_MODE) ? 1 : 0;
}

/* ------------------------------------------------------------------ 查找窗口 */

static HWND g_best;
static long g_bestArea;
static int g_bestHasEd;
static int g_scanHasEd;

/* 7.x 的主窗是两层 top-level：外层 wrapper（类名 Window，铺满全屏，里面一个
   子窗都没有）+ 真正装着编辑器的 form（TCustomForm）。只看面积会选中 wrapper，
   之后就永远找不到编辑器。更坑的是 7.x 的 form 常常 IsWindowVisible() 报 false
   （界面明明显示着），所以这里只按“里面有没有真子窗”来认。 */
static BOOL CALLBACK enumHasEd(HWND h, LPARAM lp)
{
  RECT r;
  (void)lp;
  if (!GetWindowRect(h, &r)) return TRUE;
  if ((r.right - r.left) >= 200 && (r.bottom - r.top) >= 80) {
    g_scanHasEd = 1;
    return FALSE;
  }
  return TRUE;
}

static int hostHasEditor(HWND h)
{
  g_scanHasEd = 0;
  EnumChildWindows(h, enumHasEd, 0);
  return g_scanHasEd;
}

static BOOL CALLBACK enumTop(HWND h, LPARAM lp)
{
  RECT r;
  WCHAR cls[128], title[256];
  int hasEd, vis, score;
  long area;
  (void)lp;
  if (!GetWindowRect(h, &r)) return TRUE;
  GetClassNameW(h, cls, 128);
  if (!wcscmp(cls, CLASS_GLASS)) return TRUE;
  GetWindowTextW(h, title, 256);
  if (wcsstr(title, L"DevAI")) {
    /* 小助手浮窗位置单独记下，卡片要避开它（它通常比主窗窄，不能受下面的尺寸过滤影响） */
    g_panelL = r.left; g_panelT = r.top; g_panelB = r.bottom;
    g_hPanel = h;
    return TRUE;
  }
  /* 老版（6.x）主窗类是 TMainForm，新版（7.x，Lazarus 重写）是 TCustomForm，
     两者标题里都有 “Dev-C++”。这里只按标题认宿主，尺寸门槛放宽，
     用户把窗口拉小也不至于找不到。 */
  if (!wcsstr(title, L"Dev-C++") && wcscmp(cls, L"TMainForm") && wcscmp(cls, L"TCustomForm"))
    return TRUE;
  if ((r.right - r.left) < 700 || (r.bottom - r.top) < 400) return TRUE;
  if (IsIconic(h)) return TRUE;
  hasEd = hostHasEditor(h);
  vis = IsWindowVisible(h) ? 1 : 0;
  if (!vis && !hasEd) return TRUE;
  area = (long)(r.right - r.left) * (r.bottom - r.top);
  /* 有真子窗的（7.x form）优先；其次可见的；同样条件才比面积 */
  score = hasEd * 2 + vis;
  if (score > g_bestHasEd || (score == g_bestHasEd && area > g_bestArea)) {
    g_bestArea = area; g_best = h; g_bestHasEd = score;
  }
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
  /* 7.x 的 form 经常 IsWindowVisible() 报 false（界面其实正常显示），
     这里不能靠可见性过滤，只能靠类名 + 尺寸。 */
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

/* 新版（7.x）的编辑器不是 TSynEdit：它是 Lazarus 的通用 Window 类，
   外面还套一层同尺寸的容器。找不到 TSynEdit 时按“面积最大、可见、
   占满上半屏”挑一个，面积相同时取枚举顺序里更靠后的那个（也就是更里层、
   真正负责绘制和滚动的那一层）。 */
static HWND g_fbBest;
static long g_fbArea;
static int g_fbScroll;

static int isBadEditorClass(const WCHAR *cls)
{
  static const WCHAR *bad[] = {
    L"SysTabControl32", L"SysListView32", L"SysTreeView32", L"SysHeader32",
    L"msctls_statusbar32", L"msctls_progress32", L"msctls_updown32",
    L"tooltips_class32", L"toolbarwindow32", L"ReBarWindow32", L"Button",
    L"ScrollBar", L"LCLComboBox", L"ComboBox", L"ComboLBox", L"Static",
    L"TPUtilWindow", L"MDIClient", NULL
  };
  int i;
  for (i = 0; bad[i]; i++) if (!_wcsicmp(cls, bad[i])) return 1;
  return 0;
}

static BOOL CALLBACK enumFallback(HWND h, LPARAM lp)
{
  RECT r;
  WCHAR cls[128];
  int hostW = g_hostR - g_hostL, hostH = g_hostB - g_hostT;
  (void)lp;
  if (!GetWindowRect(h, &r)) return TRUE;
  GetClassNameW(h, cls, 128);
  if (!wcscmp(cls, CLASS_GLASS)) return TRUE;
  if (isBadEditorClass(cls)) return TRUE;
  if ((r.right - r.left) < hostW / 2) return TRUE;
  if ((r.bottom - r.top) < hostH / 3) return TRUE;
  if ((r.bottom - r.top) < S(150)) return TRUE;
  long area = (long)(r.right - r.left) * (r.bottom - r.top);
  {
    /* 7.x 里编辑器外面还包了一层同样大的容器（上面带列标尺：0 10 20…）。
       容器比真编辑面还大一圈，按面积会选错 —— 选错就会把“列标尺”当成
       第一条行号，色带整体偏一行。带纵向滚动条的那个才是真编辑面，优先选它。 */
    SCROLLINFO si;
    int hasScroll = 0;
    ZeroMemory(&si, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL;
    if (GetScrollInfo(h, SB_VERT, &si) && si.nMax > 0) hasScroll = 1;
    if (hasScroll && !g_fbScroll) {
      g_fbArea = area; g_fbBest = h; g_fbScroll = 1;
    } else if (hasScroll == g_fbScroll && area > g_fbArea) {
      g_fbArea = area; g_fbBest = h; g_fbScroll = hasScroll;
    }
  }
  return TRUE;
}

static HWND findEditorFallback(HWND host)
{
  g_fbBest = NULL;
  g_fbArea = 0;
  g_fbScroll = 0;
  EnumChildWindows(host, enumFallback, 0);
  return g_fbBest;
}

/* 7.x 的编辑面外面还套了一层同尺寸的容器，滚动信息在内层子窗上。
   找一个带纵向滚动范围、面积最大的子窗来读滚动位置。 */
static HWND g_scBest;
static long g_scArea;

static BOOL CALLBACK enumScrollChild(HWND h, LPARAM lp)
{
  SCROLLINFO si;
  RECT r;
  long area;
  (void)lp;
  ZeroMemory(&si, sizeof(si));
  si.cbSize = sizeof(si);
  si.fMask = SIF_ALL;
  if (!GetScrollInfo(h, SB_VERT, &si) || si.nMax <= 0) return TRUE;
  if (!GetWindowRect(h, &r)) return TRUE;
  area = (long)(r.right - r.left) * (r.bottom - r.top);
  if (area > g_scArea) { g_scArea = area; g_scBest = h; }
  return TRUE;
}

static HWND findScrollChild(HWND parent)
{
  SCROLLINFO si;
  ZeroMemory(&si, sizeof(si));
  si.cbSize = sizeof(si);
  si.fMask = SIF_ALL;
  if (GetScrollInfo(parent, SB_VERT, &si) && si.nMax > 0) return parent;
  g_scBest = NULL;
  g_scArea = 0;
  EnumChildWindows(parent, enumScrollChild, 0);
  return g_scBest;
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
  g_bestHasEd = 0;
  g_panelL = 0; g_panelT = 0; g_panelB = 0; g_hPanel = NULL;
  EnumWindows(enumTop, 0);
  if (!g_best) return 0;
  g_hHost = g_best;
  GetWindowRect(g_hHost, &r);
  g_hostL = r.left; g_hostT = r.top; g_hostR = r.right; g_hostB = r.bottom;

  g_hEdit = findChildClass(g_hHost, L"TSynEdit", 300, 100);
  if (!g_hEdit) g_hEdit = findEditorFallback(g_hHost);   /* 7.x 的编辑器不是 TSynEdit */
  if (!g_hEdit) return 0;
  g_hScroll = findScrollChild(g_hEdit);
  if (!g_hScroll) g_hScroll = g_hEdit;
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
static int g_scrollTopLine = 0;   /* 由滚动条换算出的第一可见行（0 = 不可用） */
/* 用画面像素 + 编辑器全文拟合出来的第一可见行（0 = 没算出来）。
   老版 Dev-C++ 打开文件后滚动条可能还没同步（画面停在第 10 行、pos 还是 1），
   这个值比滚动条可靠，是“概率错行”的直接解药。 */
static int g_fitTop = 0;

/* 用编辑器的真实文本反推“当前第一可见行”：把每行字宽和画面上量到的行尾对齐 */
static void detectTopLine(int *starts, int *ends, int count)
{
  static int off[4096];
  int lc, i, t, rn = 0, bestT = 0, bestX0 = 0, bestCw = 0, scrollOK = 0;
  double bestCost = 1e18, scrollCost = 1e18;
  int scrollCostOK = 0;
  int ks[60], inkS[60], inkE[60], rowY[60], rowTop[60], rowBot[60], rowLim[60];
  int pitch = g_pitch > 0 ? g_pitch : S(22);
  int firstY = g_firstTextY;
  g_targetRowTop = g_targetRowBot = -1;
  g_fitTop = 0;
  if (!readEditorText()) return;
  lc = textLineCount(off, 4096);
  /* 滚动条换算出的第一可见行最准（6.x 直接是行号，7.x 由滚过的像素除以行距）；
     文本像素拟合在代码高度重复的文件里容易整体错几行。 */
  scrollOK = (g_scrollTopLine > 0);
  if (scrollOK) g_topLine = g_scrollTopLine;
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
    if (scrollOK && t == g_scrollTopLine) { scrollCost = cost; scrollCostOK = 1; }
    if (cost < bestCost) { bestCost = cost; bestT = t; bestX0 = (int)x0; bestCw = (int)cw; }
  }
  /* 画面拟合 vs 滚动条：两个候选都拿“行首缩进 + 行尾长度”跟画面对了一遍，
     谁更贴合用谁。老版打开文件后滚动条偶尔还没同步，照滚动条标会整体错好几行；
     但代码高度重复的文件里拟合也可能错位，所以只有拟合“明显更贴合”才推翻滚动条。 */
  if (scrollOK) {
    int useFit = 0;
    if (bestT > 0 && bestCw > 0) {
      if (!scrollCostOK) useFit = (bestCost <= S(6));
      else if (bestCost + S(3) < scrollCost) useFit = 1;
    }
    if (!useFit) bestT = g_scrollTopLine;
  }
  g_fitTop = (bestT > 0 && bestCw > 0) ? bestT : 0;
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
    /* 出错行如果就在画面里，直接用这一行真实的文字上下沿，色带就不会上下漂。
       怎么认这一行？不看“第几个文字行”（空行会让序号错位），也不全信行距，
       而是拿源码里这一行的“行首缩进 + 行尾长度”去和画面上每条墨迹比对：
       缩进一致、行尾长度一致的，就是同一行。位置只作为就近筛选条件。 */
    if (gd.line > 0) {
      int want = gd.line - bestT;
      int li = gd.line - 1;
      int pred = g_firstTextY + want * pitch;
      if (want >= 0 && li >= 0 && li < lc) {
        int sOff = off[li];
        int eOff = (li + 1 < lc) ? off[li + 1] : g_editTextLen;
        int vi = 0, vlen = lineVisual(sOff, eOff, &vi);
        int expS = g_codeX0 + g_charW * vi;
        int expE = g_codeX0 + g_charW * vlen;
        int best = -1;
        double bestC = 1e18;
        int foundColored = 0;
        /* Dev-C++ 把出错行涂成红底（或把选中的行涂成蓝底）时，这一行在
           文字行扫描里是被“跳过”的，指纹匹配就只能匹配到上一行/下一行。
           所以先直接认变色行：中心落在推算行内的那条变色行就是它。 */
        {
          int ci, cbest = -1, cd = 1 << 30, midPred = pred + pitch / 2;
          for (ci = 0; ci < g_colRowN; ci++) {
            int mid = (g_colRowTop[ci] + g_colRowBot[ci]) / 2;
            int d = mid - midPred;
            if (d < 0) d = -d;
            if (d < cd && d * 3 <= pitch * 2) { cd = d; cbest = ci; }
          }
          if (cbest >= 0) {
            g_targetRowTop = g_colRowTop[cbest];
            g_targetRowBot = g_colRowBot[cbest];
            foundColored = 1;
          }
        }
        for (i = 0; !foundColored && i < rn; i++) {
          int d = rowTop[i] - pred, cost;
          if (d < 0) d = -d;
          if (d > pitch * 2) continue;          /* 只在推算位置前后两行内找 */
          if (vlen >= 4) {
            cost = abs(inkS[i] - expS) * 2 + abs(inkE[i] - expE);
            if (cost < bestC) { bestC = cost; best = i; }
          } else {
            /* 空行/极短行没有行尾可比，退回按位置取最近的一条 */
            if (d * 3 < bestC) { bestC = d * 3; best = i; }
          }
        }
        if (best >= 0 && bestC <= S(120) && rowTop[best] <= rowBot[best]) {
          g_targetRowTop = rowTop[best];
          g_targetRowBot = rowBot[best];
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
  /* 抓屏时不能把自己画的高亮/提示条抓进去。
     系统支持“窗口不参与截图”时（Win10 2004+）什么都不用做；
     老系统只能先藏起来 70ms 再抓 —— 这一步会闪，属于兜底方案。 */
  if (!g_capExclOk) {
    if (g_hBand && IsWindowVisible(g_hBand)) { ShowWindow(g_hBand, SW_HIDE); g_capFallback = 1; }
    if (g_hCard && IsWindowVisible(g_hCard)) { ShowWindow(g_hCard, SW_HIDE); g_capFallback = 1; }
    if (g_hStrip && IsWindowVisible(g_hStrip)) { ShowWindow(g_hStrip, SW_HIDE); g_capFallback = 1; }
    if (g_capFallback) Sleep(70);
  }
  screen = GetDC(NULL);
  BitBlt(g_cap.hdc, 0, 0, w, h, screen, g_edL, g_edT, SRCCOPY);
  ReleaseDC(NULL, screen);
  return 1;
}

/* ---- 行号栏扫描（重做版） ----
   行号栏是唯一可靠的“行坐标”：Dev-C++ 每一行都画行号，空行、红行、蓝行都不缺。
   1) 用最左一列的颜色当行号栏底色，据此找出行号栏 / 代码区的分界；
   2) 逐行统计数字笔画（比行号栏底色暗 14 以上的像素数），>=4 个才算一行数字。
      左边的折叠线只有 1~2 像素宽，这样自然被排除（旧版就是被它搅成一整块）；
   3) 全部数字带的上沿做线性回归 → 精确行距（缺几条也不影响）；
   4) 相位 = 每条带上沿减掉“第几条×行距”后的平均。
   结果：g_gutPitch 行距、g_gutPhase 相位、g_gutDigitH 数字高度。 */
static void scanGutterBands(Canvas *c, int cw, int ch, int xlim,
                            int bgR, int bgG, int bgB)
{
  int x, y, i, n, run = 0;
  static int rowCnt[2048];
  int top[GUT_MAX], bot[GUT_MAX], rn = 0;
  int lumThr, codeLeft, gbR, gbG, gbB;
  (void)bgR; (void)bgG; (void)bgB;
  g_gutN = 0; g_gutOK = 0; g_gutPitch = 0; g_gutPhase = 0; g_gutDigitH = 0;
  if (!c || cw < 40 || ch < 40) return;
  if (xlim > cw) xlim = cw;
  lumThr = 0; codeLeft = 0;

  /* 1) 代码区底色 = 全图最亮的那个大峰（白底主题就是 255）。
        行号栏底色一般比它暗一档（240），所以墨迹阈值取“亮底 - 25”：
        行号栏底色不会被当成字，而 10/11/12 那种浅灰行号仍然算字。 */
  {
    static int hs[256];
    long n2 = 0;
    for (i = 0; i < 256; i++) hs[i] = 0;
    for (y = 0; y < ch; y += 2)
      for (x = 0; x < cw && x < S(400); x += 2) {
        unsigned int v = c->px[(size_t)y * cw + x];
        int r = (v >> 16) & 255, g2 = (v >> 8) & 255, b = v & 255;
        hs[(r + g2 + b) / 3]++;
        n2++;
      }
    if (n2 < 100) return;
    lumThr = 0;
    for (i = 255; i >= 0; i--) if (hs[i] * 10 >= n2) { lumThr = i; break; }
    if (lumThr < 160) { glog(L"gut bail dark lum=%d", lumThr); return; }
    gbR = gbG = gbB = lumThr;
    lumThr -= 25;
    if (lumThr < 80) lumThr = 80;
  }

  /* 2) 行号栏 / 代码区的分界：连续 10 列大部分像素都接近亮底 → 从这里进代码区 */
  {
    int lim = xlim < S(300) ? xlim : S(300);
    int run2 = 0, bright = gbR;
    codeLeft = lim;
    for (x = S(6); x < lim; x++) {
      int cntb = 0, k = 0;
      for (y = ch / 6; y < ch * 5 / 6; y += 3) {
        unsigned int v = c->px[(size_t)y * cw + x];
        int r = (v >> 16) & 255, g2 = (v >> 8) & 255, b = v & 255;
        if (abs((r + g2 + b) / 3 - bright) <= 6) cntb++;
        k++;
      }
      if (k && cntb * 100 > k * 55) {
        if (++run2 >= S(10)) { codeLeft = x - run2 + 1; break; }
      } else run2 = 0;
    }
    if (codeLeft < S(24)) codeLeft = S(24);
  }

  /* 2.5) 行号栏底色跟代码区不是一个亮度：6.x 是 240，7.x 是 224。
     墨迹阈值必须相对行号栏自己的底色算（取众数），否则 7.x 整条行号栏
     都会被算成“字”，所有行黏成一大块。 */
  {
    static int gh[256];
    long gn2 = 0;
    int mode = 0;
    for (i = 0; i < 256; i++) gh[i] = 0;
    for (y = ch / 6; y < ch * 5 / 6; y += 2)
      for (x = S(4); x < codeLeft - S(2); x++) {
        unsigned int v = c->px[(size_t)y * cw + x];
        int r = (v >> 16) & 255, g2 = (v >> 8) & 255, b = v & 255;
        gh[(r + g2 + b) / 3]++;
        gn2++;
      }
    if (gn2 < 50) { glog(L"gut bail nogutter codeLeft=%d", codeLeft); return; }
    for (i = 0; i < 256; i++) if (gh[i] > mode) mode = gh[i];  /* 只用来找峰值位置 */
    lumThr = 0;
    for (i = 0; i < 256; i++) if (gh[i] == mode) { lumThr = i; break; }
    lumThr -= 15;
    if (lumThr < 80) lumThr = 80;
    if (lumThr > gbR - 8) lumThr = gbR - 8;
  }

  /* 3) 逐行统计数字笔画。每一行先取本行的中位亮度当背景，再数比背景暗 15
     以上的像素 —— Dev-C++ 会把“当前行”的整条行号栏涂灰，用固定阈值时
     这一整行都会被算成字，整幅测量就被它带偏。
     7.x 的行号笔画很细，一行常常只有 2~3 个深色像素，所以阈值取 3。 */
  {
    int gx0 = S(4), gx1 = codeLeft - S(2);
    static int colDark[8192];
    static int rowThr[2048];
    int rows = ch < 2048 ? ch : 2048;
    if (gx1 <= gx0 + 4) gx1 = gx0 + 5;
    if (gx1 > cw) gx1 = cw;
    if (gx1 > 8192) gx1 = 8192;
    for (i = 0; i < gx1; i++) colDark[i] = 0;
    for (y = 0; y < rows; y++) {
      static int rh[256];
      int acc = 0, med = 0, m = 0, cnt = gx1 - gx0;
      int bgDark, thr;
      for (i = 0; i < 256; i++) rh[i] = 0;
      for (x = gx0; x < gx1; x++) {
        unsigned int v = c->px[(size_t)y * cw + x];
        int r = (v >> 16) & 255, g2 = (v >> 8) & 255, b = v & 255;
        rh[(r + g2 + b) / 3]++;
      }
      for (i = 0; i < 256; i++) { acc += rh[i]; if (acc * 2 >= cnt) { med = i; break; } }
      if (med <= 0) { rowCnt[y] = 0; rowThr[y] = 0; continue; }
      /* 6.x 的行号栏里除了 224 的底色，还有一条 200 的折叠边栏。它比底色暗，
         用“中位亮度-15”会把整条边栏都算成笔画，所有行黏成一整块。
         取本行两个主要亮度里较暗的那个当背景，才是真正的底色。 */
      {
        int b2 = 0, b2c = 0, need = cnt / 8;
        if (need < 2) need = 2;
        for (i = 0; i < med - 7; i++)
          if (rh[i] > b2c) { b2 = i; b2c = rh[i]; }
        bgDark = (b2c >= need && b2 * 4 >= med * 3) ? b2 : med;
      }
      thr = bgDark - 15;
      rowThr[y] = thr;
      for (x = gx0; x < gx1; x++) {
        unsigned int v = c->px[(size_t)y * cw + x];
        int r = (v >> 16) & 255, g2 = (v >> 8) & 255, b = v & 255;
        if ((r + g2 + b) / 3 < thr) { m++; if (x < 8192) colDark[x]++; }
      }
      rowCnt[y] = m;
    }
    /* 折叠栏的竖线、边框：几乎每一行都有 1~2 个深色像素，会被当成“每一行都有字”。
       这种整列去掉，剩下的才是数字笔画。 */
    for (x = gx0; x < gx1; x++)
      if (colDark[x] * 10 >= rows * 6) colDark[x] = -1;
    for (y = 0; y < rows; y++) {
      int m2 = 0;
      if (rowThr[y] <= 0) { rowCnt[y] = 0; continue; }
      for (x = gx0; x < gx1; x++) {
        unsigned int v;
        int r, g2, b;
        if (colDark[x] == -1) continue;
        v = c->px[(size_t)y * cw + x];
        r = (v >> 16) & 255; g2 = (v >> 8) & 255; b = v & 255;
        if ((r + g2 + b) / 3 < rowThr[y]) m2++;
      }
      rowCnt[y] = m2;
    }
  }
  for (y = 0; y < ch && y < 2048; y++) {
    if (rowCnt[y] >= 2) {
      if (!run) { if (rn < GUT_MAX) { top[rn] = y; bot[rn] = y; rn++; } run = 1; }
      else if (rn) bot[rn - 1] = y;
    } else run = 0;
  }
  /* 只留“一条数字”那么高的带子：更矮的是噪点/折叠框，更高的是整片底色 */
  for (i = 0, n = 0; i < rn; i++) {
    int h2 = bot[i] - top[i] + 1;
    if (h2 < S(5) || h2 > S(46)) continue;
    top[n] = top[i]; bot[n] = bot[i]; n++;
  }
  rn = n;
  if (rn < 6) { glog(L"gut bail rn=%d codeLeft=%d", rn, codeLeft); return; }

  /* 4) 行距：带子上沿（字顶）的间隔中位数 + 等距网格校验 */
  {
    int gaps[256], gn = 0, k, med, a, b2;
    int idx[GUT_MAX], last = 0, use[GUT_MAX];
    int anchor, pitch, good = 0;
    int hs[256], hn = 0;
    for (i = 1; i < rn && gn < 256; i++) {
      int gp = top[i] - top[i - 1];
      if (gp >= S(10) && gp <= S(80)) gaps[gn++] = gp;
    }
    if (gn < 5) return;
    for (a = 1; a < gn; a++) {
      int v = gaps[a];
      for (b2 = a - 1; b2 >= 0 && gaps[b2] > v; b2--) gaps[b2 + 1] = gaps[b2];
      gaps[b2 + 1] = v;
    }
    med = gaps[gn / 2];
    if (med < S(10) || med > S(60)) return;
    for (i = 0; i < rn; i++) {
      k = (top[i] - top[0] + med / 2) / med;
      idx[i] = k;
      if (k > last) last = k;
      use[i] = 1;
    }
    if (last < 4) return;
    /* 锚点用“字顶 - 第几条×行距”的中位数：个别带子被噪声切开也不会把它带偏 */
    {
      int phs[GUT_MAX], pn = 0;
      for (i = 0; i < rn; i++) phs[pn++] = top[i] - idx[i] * med;
      for (a = 1; a < pn; a++) {
        int v = phs[a];
        for (b2 = a - 1; b2 >= 0 && phs[b2] > v; b2--) phs[b2 + 1] = phs[b2];
        phs[b2 + 1] = v;
      }
      anchor = phs[pn / 2];
    }
    /* 最小二乘把行距精确到整数（整除误差会被行数放大），两轮剔除离群带 */
    {
      double pp = med, aa = anchor;
      int pass;
      for (pass = 0; pass < 3; pass++) {
        double sk = 0, sy = 0, skk = 0, sky = 0;
        int m = 0;
        for (i = 0; i < rn; i++) {
          if (!use[i]) continue;
          k = (int)floor((top[i] - aa) / pp + 0.5);
          idx[i] = k;
          sk += k; sy += top[i]; skk += (double)k * k; sky += (double)k * top[i];
          m++;
        }
        if (m < 5) break;
        {
          double den = m * skk - sk * sk;
          if (den > 0.5) { pp = (m * sky - sk * sy) / den; aa = (sy - pp * sk) / m; }
        }
        for (i = 0; i < rn; i++) {
          double d = top[i] - (aa + pp * idx[i]);
          if (d < 0) d = -d;
          use[i] = d <= 4 ? 1 : 0;
        }
      }
      pitch = (int)(pp + 0.5);
      if (pitch < S(10) || pitch > S(60)) return;
      anchor = (int)(aa + 0.5);
    }
    for (i = 0; i < rn; i++) if (use[i]) good++;
    if (good < 6 || good * 10 < rn * 6) return;
    g_gutPitch = pitch;
    g_gutFirstTop = anchor;          /* 第一可见行的字顶（绝对 y） */
    g_gutPhase = anchor % pitch;
    /* 5) 数字高度中位数（只用对上的带子）：色带靠它在行内摆正 */
    for (i = 0; i < rn; i++) if (use[i] && hn < 256) hs[hn++] = bot[i] - top[i] + 1;
    for (a = 1; a < hn; a++) {
      int v = hs[a];
      for (b2 = a - 1; b2 >= 0 && hs[b2] > v; b2--) hs[b2 + 1] = hs[b2];
      hs[b2 + 1] = v;
    }
    g_gutDigitH = hn > 0 ? hs[hn / 2] : pitch / 2;
    for (i = 0; i < rn && i < GUT_MAX; i++) { g_gutTop[i] = top[i]; g_gutBot[i] = bot[i]; }
    g_gutN = rn;
    g_gutOK = 1;
  }
}

/* 调试：把刚抓到的编辑区存成 BMP，方便离线看行号栏长什么样 */
static void dumpCaptureBmp(void)
{
  WCHAR flag[MAX_PATH], out[MAX_PATH];
  BITMAPFILEHEADER fh;
  BITMAPINFOHEADER ih;
  HANDLE f;
  DWORD wr = 0;
  int y, rowBytes;
  wsprintfW(flag, L"%s\\state\\dumpcap.txt", gHome);
  if (GetFileAttributesW(flag) == INVALID_FILE_ATTRIBUTES) return;
  if (!g_capReady) return;
  wsprintfW(out, L"%s\\state\\cap.bmp", gHome);
  rowBytes = g_capW * 4;
  ZeroMemory(&fh, sizeof(fh));
  ZeroMemory(&ih, sizeof(ih));
  ih.biSize = sizeof(ih);
  ih.biWidth = g_capW;
  ih.biHeight = g_capH;
  ih.biPlanes = 1;
  ih.biBitCount = 32;
  ih.biCompression = BI_RGB;
  ih.biSizeImage = rowBytes * g_capH;
  fh.bfType = 0x4D42;
  fh.bfOffBits = sizeof(fh) + sizeof(ih);
  fh.bfSize = fh.bfOffBits + ih.biSizeImage;
  f = CreateFileW(out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE) return;
  WriteFile(f, &fh, sizeof(fh), &wr, NULL);
  WriteFile(f, &ih, sizeof(ih), &wr, NULL);
  for (y = g_capH - 1; y >= 0; y--)
    WriteFile(f, g_cap.px + (size_t)y * g_capW, rowBytes, &wr, NULL);
  CloseHandle(f);
  DeleteFileW(flag);
  glog(L"dump cap.bmp %dx%d", g_capW, g_capH);
}

/* 通过扫描像素找到“文字行”的起点与行距 */
static void measureLines(void)
{
  int w = g_capW, h = g_capH, x, y, i, k, sw, x0;
  int bgR = 255, bgG = 255, bgB = 255;
  int starts[80], ends[80], count = 0;
  int thr, minH, limR;
  g_colRowN = 0;
  if (!g_capReady) { g_pitch = S(22); g_firstTextY = S(6); g_baseY = 0; g_inkRight = 0; g_lastCodeY = 0; return; }
  dumpCaptureBmp();
  sw = w / 2;
  if (sw < 300) sw = w;
  x0 = S(70);
  if (x0 > sw - 60) x0 = 0;
  limR = w - S(40);
  /* 助手面板浮在编辑区右边，它的边框+阴影有上百像素宽，会被误当成“代码墨迹”，
     把提示条/讲解卡顶到最左边压在代码上 —— 这里多让开一段 */
  if (g_panelL > g_edL && g_panelL - g_edL - S(110) < limR) limR = g_panelL - g_edL - S(110);
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

  /* 行右侧的“实心底色”判定区：正常代码很少写到这么靠右，
     这一片如果被同一种颜色整片铺满，基本就是 Dev-C++ 给整行涂的底色
     （选中行蓝底、错误行红底、断点行等）。 */
  int solidX0 = w - (w * 32) / 100;
  if (solidX0 < x0 + S(40)) solidX0 = x0 + S(40);
  if (solidX0 > limR - S(20)) solidX0 = limR - S(20);
  if (solidX0 < x0) solidX0 = x0;

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
    int cnt = 0, samples = 0;
    int fillN = 0, fillHit = 0, fillKey = -1, fillSame = 1;
    int fillR = 0, fillG = 0, fillB = 0;
    for (x = x0; x < sw; x += 2) {
      unsigned int v = g_cap.px[(size_t)y * w + x];
      int r = (v >> 16) & 255, g = (v >> 8) & 255, b = v & 255;
      int d = abs(r - bgR) + abs(g - bgG) + abs(b - bgB);
      samples++;
      if (d > 240) {
        cnt++; if (x > maxInkX) maxInkX = x;
      }
    }
    /* 再看行右侧：整片同色 = Dev-C++ 涂的整行底色 */
    for (x = solidX0; x < limR; x += 3) {
      unsigned int v = g_cap.px[(size_t)y * w + x];
      int r = (v >> 16) & 255, g = (v >> 8) & 255, b = v & 255;
      int d = abs(r - bgR) + abs(g - bgG) + abs(b - bgB);
      int key = ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
      fillN++;
      if (d > 16) {
        fillHit++;
        if (fillKey < 0) { fillKey = key; fillR = r; fillG = g; fillB = b; }
        else if (key != fillKey) fillSame = 0;
      }
    }
    if (y < hi) prof[y] = cnt;
    /* Dev-C++ 给某一行“变色”（红色错误行、蓝色选中行等）时，整行都是底色，
       会被误当成一行文字，把行距/首行相位算歪，导致我们的标行整体错位。
       两个判据任一成立就整行跳过：整行墨迹覆盖率 >65%，或行右侧 88% 以上
       被同一种非底色铺满。 */
    {
      int coloredRow = 0;
      if (samples > 0 && cnt * 100 > samples * 65) coloredRow = 1;
      if (fillN > 30 && fillSame && fillHit * 100 > fillN * 80) coloredRow = 1;
      if (coloredRow) {
        /* 记下这一行的上下沿和底色类型：色带要正好压在 Dev-C++ 自己的
           红色错误行 / 蓝色当前行上，而不是靠行距推算猜位置 */
        int kind = 0;
        if (fillR > 150 && fillR - fillG > 10 && fillR - fillB > 10) kind = 1;      /* 红/粉 */
        else if (fillB > 140 && fillB - fillR > 7) kind = 2;                        /* 蓝 */
        else if (fillR > 140 && fillG > 140 && fillB > 110 &&
                 (fillB - fillR > 3 || fillR - fillG > 3)) kind = 3;                /* 浅黄/浅蓝 */
        if (kind) {
          if (g_colRowN > 0 && y - g_colRowBot[g_colRowN - 1] <= S(2) &&
              g_colRowKind[g_colRowN - 1] == kind) {
            g_colRowBot[g_colRowN - 1] = y;
          } else if (g_colRowN < 32) {
            g_colRowTop[g_colRowN] = y;
            g_colRowBot[g_colRowN] = y;
            g_colRowKind[g_colRowN] = kind;
            g_colRowN++;
          }
        }
        if (y < hi) prof[y] = 0;
        inBand = 0;
        continue;
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
      /* 超过 3/4 的行都有墨的竖带也不是文字（面板阴影、边框） */
      if (hitRows >= 2 && hitRows * 4 < count * 3 && x > cRight) cRight = x;
    }
    if (cRight > 0) inkRight = cRight;
  }
  if (count == 0) {
    /* 一行字都没扫到：多半是锁屏/窗口被遮住，抓到的是一张空图。
       已经量准过就保留原值，别把好数据覆盖成默认值（否则一恢复就整体错位）；
       同时要求下一轮重新量，恢复后能自动补上 */
    g_forceMeasure = 1;
    if (g_pitch > S(8)) return;
    g_pitch = S(22); g_firstTextY = S(6); g_baseY = 0; g_inkRight = inkRight; g_lastCodeY = 0;
    if (g_codeX0 <= 0) g_codeX0 = S(56);
    if (g_charW <= 0) g_charW = S(9);
    return;
  }

  /* ---- 行号栏：每一行都画着一个行号，数字的上下沿就是这一行的上下沿 ---- */
  scanGutterBands(&g_cap, w, h, w, bgR, bgG, bgB);

  /* 行号栏量出来的行距是精确值（整数像素），相位也是精确的：直接采用。
     之后滚动只用“滚动条位移”推算，绝不累加，所以不会越滚越偏。 */
  int keepGeo = 0;
  if (g_gutOK && g_gutPitch > S(8)) {
    int adopt = 1, pad;
    if (!g_measureTrust && g_pitch > S(8)) {
      int d = g_gutPitch - g_pitch;
      if (d < 0) d = -d;
      if (d * 100 > g_pitch * 12) adopt = 0;   /* 行距突然变了：先不信这一次 */
    }
    if (adopt) g_pitch = g_gutPitch;
    pad = (g_pitch - (g_gutDigitH > 0 ? g_gutDigitH : g_pitch / 2)) / 2;
    if (pad < 0) pad = 0;
    /* 第一可见行的行顶 = 它的数字带上沿 - 行内留白（测量当时的屏幕 y） */
    g_firstTextY = g_gutFirstTop - pad;
    /* 顶行的数字被裁掉一半时（像素滚动到半行），它的上沿不可信，用下一行倒推。
       只有“下一行确实隔着一个行距”才认，防止最上面那条是杂带时把基准带偏。 */
    if (g_gutFirstTop <= 1 && g_gutN > 1) {
      int d2 = g_gutTop[1] - g_gutTop[0];
      if (d2 >= g_pitch / 2 && d2 <= g_pitch * 3 / 2)
        g_firstTextY = g_gutTop[1] - pad - g_pitch;
    }
    /* 换算成“第 1 行在文档里的 y”：第一可见行 = 1 + 整行滚过的行数，
       所以在屏幕 y 上只差“不足一行的余数”。之后每一帧
       y = baseY + (行号-1)×行距 - 当前滚过的像素，纯绝对量，绝不会越滚越偏。
       只在“刚量准/尺寸变了”或“抓屏时没有滚动”时才更新基准：
       滚动途中抓到的画面可能和滚动位置差一帧，拿它更新基准会留下半行误差。 */
    if (g_measureTrust || g_baseY <= 0 || g_measScrollStable)
      g_baseY = g_firstTextY + (g_pitch > 0 ? g_scrollPx % g_pitch : 0);
    keepGeo = 1;
  } else if (!g_measureTrust && g_baseY > 0 && g_pitch > S(8)) {
    /* 这次行号栏没扫到（窗口被别的程序盖住 / 正在滚动 / 画面偏暗），
       但之前已经量准过：直接沿用旧几何，绝不能用自相关的结果覆盖 ——
       那种结果时不时会差半行或一整行，正是“滑回顶部就偏一行”的来源。 */
    keepGeo = 1;
  } else {
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
    /* 自相关偶尔会锁到半周期/谐波上（尤其是屏幕上正好有 Dev-C++ 的整行红底高亮时），
       这时标行会整体错位。用相邻文字行的实际间距中位数校正一下；
       样本够多时直接用中位数（空白行只会给出整数倍，中位数仍落在行距上）。 */
    {
      int gaps[64], gn = 0, a, b2, med = 0;
      for (i = 1; i < count && gn < 64; i++) {
        int gp = starts[i] - starts[i - 1];
        if (gp >= S(10) && gp <= S(60)) gaps[gn++] = gp;
      }
      if (gn >= 3) {
        for (a = 1; a < gn; a++) {
          int v = gaps[a];
          for (b2 = a - 1; b2 >= 0 && gaps[b2] > v; b2--) gaps[b2 + 1] = gaps[b2];
          gaps[b2 + 1] = v;
        }
        /* 相邻文字行的间距中位数。空行只会给出整数倍间距，中位数仍落在行距上；
           注意不能用最小值 —— 行的“起点”取决于这一行最高的那个字母，
           有的行高有的行矮，最小值会偏小（实测 26 vs 真行距 33）。 */
        med = gaps[gn / 2];
        if (med >= S(10) && med <= S(60) &&
            med * 100 >= pitch * 85 && med * 100 <= pitch * 115) {
          pitch = (pitch + med + 1) / 2;
        }
      }
    }
    /* 行距一旦量准就不再随便改。滚动、Dev-C++ 给某行变色都会触发重测，
       重测如果给出明显不同的值，一律保留旧值 —— 否则标行会整体跳到别的行上。
       只有窗口尺寸变了（字体/缩放真的变了）或还没量过时才接受新值。 */
    if (g_pitch > S(8) && !g_measureTrust) {
      int d = pitch - g_pitch;
      if (d < 0) d = -d;
      if (count < 6 || d * 100 > g_pitch * 12) {
        pitch = g_pitch; keepGeo = 1;
      }
    }
    g_pitch = pitch;
  }
  }   /* end: 没有行号栏时才走自相关 */

  /* 首行相位：让每一行的文字顶都落在同一个偏移上（文字最密的那一行对齐） */
  if (!keepGeo) {
    int p, bestP = 0;
    long long bestS = -1;
    for (p = 0; p < g_pitch; p++) {
      long long s = 0;
      for (y = p; y < hi; y += g_pitch) s += prof[y];
      if (s > bestS || (s == bestS && p < bestP)) { bestS = s; bestP = p; }
    }
    /* 相位整体乱跳（多半是变色行把轮廓搅了）也不采用 */
    {
      int d = bestP - g_firstTextY;
      if (d < 0) d = -d;
      if (g_firstTextY > 0 && d > g_pitch / 2 && !g_measureTrust) bestP = g_firstTextY;
    }
    g_firstTextY = bestP;
  }
  if (!keepGeo && g_firstTextY < 0) g_firstTextY = 0;
  if (!keepGeo) g_baseY = 0;   /* 没拿到行号栏网格：退回旧算法 */
  detectTopLine(starts, ends, count);
  /* 变色行（Dev-C++ 的红色错误行 / 蓝色选中行）换算成绝对行号：
     色带如果正好标的是这一行，就直接用它的真实上下沿，像素级贴合。 */
  {
    int ci, pl = g_pitch > S(8) ? g_pitch : S(22);
    for (ci = 0; ci < g_colRowN; ci++) {
      int mid = (g_colRowTop[ci] + g_colRowBot[ci]) / 2;
      int r = (mid - g_firstTextY) / pl;
      if (r < 0) r = 0;
      g_colLine[ci] = g_topLine + r;
    }
  }
  if (g_codeX0 <= 0) g_codeX0 = S(56);
  if (g_charW <= 0) g_charW = S(9);
  g_inkRight = inkRight > 0 ? inkRight : maxInkX;
  g_lastCodeY = (lastInk > 0) ? lastInk + S(4) : 0;
}

static int readTopLine(void)
{
  SCROLLINFO si;
  int edH = g_edB - g_edT;
  g_scrollTopLine = 0;
  ZeroMemory(&si, sizeof(si));
  si.cbSize = sizeof(si);
  si.fMask = SIF_ALL;
  if (g_hScroll && GetScrollInfo(g_hScroll, SB_VERT, &si)) {
    /* 老版（6.x，TSynEdit）的纵向滚动条按“行”走：nPos 就是第一可见行（1 起）、
       nMax 是总行数、nPage 是一屏行数（几十）。
       新版（7.x）是“像素滚动”：nPos 是滚过的像素数、nPage 是一屏像素数（几百），
       要除以行距才是第一可见行。 */
    g_scrollTop = si.nPos;
    g_scrollLines = si.nMax;
    g_scrollPage = si.nPage;
    if (si.nPage > 120) {
      /* 像素滚动：nPos 本身就是滚过的像素，和行距同一套坐标。
         第一可见行 = 1 + 已经整行滚过去的行数（向下取整，别四舍五入，
         否则滚到半行时会来回跳一行）。 */
      g_scrollPx = si.nPos > 0 ? si.nPos : 0;
      if (g_pitch > S(8))
        g_scrollTopLine = 1 + g_scrollPx / g_pitch;
      else
        g_scrollTopLine = 1;
    } else {
      g_scrollTopLine = si.nPos > 0 ? (int)si.nPos : 1;
      g_scrollPx = (g_scrollTopLine - 1) * (g_pitch > 0 ? g_pitch : 0);
    }
  } else {
    g_scrollTop = 0; g_scrollLines = 0; g_scrollPage = 0;
    g_scrollPx = 0;
  }
  return g_scrollTopLine > 0 ? g_scrollTopLine : 1;
}

/* 当前第一可见行：有锚点就按“滚了多少”推（精确、跟手）；
   没有锚点才退回滚动条绝对换算。 */
static int topLineNow(void)
{
  int d, q;
  /* 滚到最上面：第一可见行永远是第 1 行（锚点推算在半行余数上可能抖一行） */
  if (g_scrollPx <= 0) return 1;
  if (!g_ancValid || g_pitch <= S(8))
    return g_scrollTopLine > 0 ? g_scrollTopLine : 1;
  d = g_scrollTop - g_ancPos;
  if (g_scrollPage > 120) {
    /* 像素滚动：向下过程中，滚满一个行距才换下一行 */
    if (d >= 0) q = d / g_pitch;
    else q = -((-d + g_pitch - 1) / g_pitch);
  } else {
    q = d;   /* 6.x 的 nPos 本来就是“第几行” */
  }
  q += g_ancLine;
  return q > 0 ? q : 1;
}

/* 用“第一可见行 + 滚动条位置”这一对重新标定 */
static void anchorScrollAt(int line)
{
  g_ancLine = line > 0 ? line : 1;
  g_ancPos = g_scrollTop;
  g_ancValid = 1;
}

static void anchorScroll(void)
{
  anchorScrollAt(g_scrollTopLine);
}

/* ------------------------------------------------------------------ 绘制：编辑区玻璃 */

static int g_bandLineY = -1, g_bandLineH = 0;
/* 这一行是不是 Dev-C++ 自己涂了底色的行（红/蓝）：是就整行铺满，一眼能看到 */
static int g_bandFullRow = 0;
/* 色带窗口实际的屏幕位置和大小：只覆盖色带那一小块，
   不再铺满整个编辑区，避免和右侧助手浮窗叠在一起互相刷 */
static int g_bandWinX = 0, g_bandWinY = 0, g_bandWinW = 0, g_bandWinH = 0;
/* 色带相对编辑区左上角的偏移：窗口一移动就按这个偏移跟过去，
   不用依赖“上次的绝对坐标”（否则 Dev-C++ 动一下色带就留在原地） */
static int g_bandRelX = 0, g_bandRelY = 0;

static void renderBand(void)
{
  Canvas c;
  int edW = g_edR - g_edL, edH = g_edB - g_edT;
  int line = gd.line;
  int bandY = -1, bandH = 0;
  int bx0 = 0, bx1 = 0, cw, ch;
  int xl = -1, xr = -1;

  if (edW < 40 || edH < 40) return;

  if (line > 0 && g_bandLineY >= 0) {
    bandY = g_bandLineY;
    bandH = g_bandLineH;
    /* 只标“有代码的那一段”，右边空白不涂 */
    rowInkRange(bandY, bandH, &xl, &xr, NULL);

    /* 左边从头：行号栏里那条小红条也要包进来 */
    bx0 = g_codeX0 > S(16) ? g_codeX0 - S(9) : S(2);
    if (bx0 < S(2)) bx0 = S(2);

    if (xl >= 0 && xr >= xl) {
      int bx = xl - S(5);
      /* 有缩进的行也别让色带从半中间开始：统一从代码第 0 列起 */
      if (g_codeX0 > 0 && bx > g_codeX0) bx = g_codeX0 - S(5);
      if (bx < S(2)) bx = S(2);
      if (bx < bx0) bx0 = bx;
      bx1 = xr + S(6);
      /* 横跨整行：只涂代码那几个字太细，实际屏幕上根本看不见。
         右边会被助手浮窗/卡片收边，所以不会盖到它们。 */
      bx1 = edW;
    } else {
      int bx = g_codeX0 > 0 ? g_codeX0 : S(52);
      if (bx < S(2)) bx = S(2);
      if (bx < bx0) bx0 = bx;
      bx1 = edW;
    }

    /* 右边别盖到助手浮窗上：卡片左边就收住 */
    if (g_cardX > g_edL) {
      int cardL = g_cardX - g_edL;
      if (cardL > bx0 && cardL < bx1) bx1 = cardL - S(6);
    }
    /* 也别盖到独立的助手面板上 */
    if (g_panelL > g_edL) {
      int pl = g_panelL - g_edL;
      if (pl > bx0 && pl < bx1) bx1 = pl - S(6);
    }

    if (bx1 > edW) bx1 = edW;
    if (bx0 >= edW) bx0 = 0;
    if (bx1 - bx0 < S(8)) {
      if (bx0 + S(8) <= edW) bx1 = bx0 + S(8);
      else bx0 = bx1 - S(8);
    }
    if (bx1 <= bx0) { bx0 = 0; bx1 = edW; }
  }

  if (bandY < 0) { hideWindow(g_hBand); return; }

  cw = bx1 - bx0;
  ch = bandH;
  if (cw < S(4) || ch < S(4)) { hideWindow(g_hBand); return; }
  if (!cvCreate(&c, cw, ch)) return;

  {
    /* 画布原点已经挪到 (bx0, bandY)，下面全部用相对坐标 */
    int gx = g_codeX0 > S(16) ? g_codeX0 - S(9) : S(2);
    if (gx < S(2)) gx = S(2);

    /* 行号栏里竖一条小红条：不压代码，又能一眼看到是哪一行 */
    if (gx >= bx0 && gx - bx0 < cw)
      cvFill(&c, gx - bx0, 0, S(5), ch, 198, 58, 58, 245);

    if (xl >= 0 && xr >= xl) {
      int bx = xl - S(5), bw2;
      if (g_codeX0 > 0 && bx > g_codeX0) bx = g_codeX0 - S(5);
      if (bx < S(2)) bx = S(2);
      if (bx < bx0) bx = bx0;
      bw2 = bx1 - bx;
      if (bw2 > cw - (bx - bx0)) bw2 = cw - (bx - bx0);
      if (bw2 > 0) {
        cvFill(&c, bx - bx0, 0, bw2, ch, 176, 52, 60, 88);            /* 淡底色 */
        cvFill(&c, bx - bx0, 0, bw2, S(2), 176, 52, 60, 150);         /* 顶线 */
        cvFill(&c, bx - bx0, ch - S(2), bw2, S(2), 176, 52, 60, 236); /* 底线 */
      }
    } else {
      /* 空行：只给个很小的记号，不横跨整行 */
      int bx = g_codeX0 > 0 ? g_codeX0 : S(52);
      if (bx < S(2)) bx = S(2);
      if (bx < bx0) bx = bx0;
      if (bx - bx0 + S(14) <= cw) {
        cvFill(&c, bx - bx0, 0, S(14), ch, 176, 52, 60, 55);
        cvFill(&c, bx - bx0, ch - S(2), S(14), S(2), 176, 52, 60, 190);
      }
    }
  }

  g_bandRelX = bx0;
  g_bandRelY = bandY;
  g_bandWinX = g_edL + bx0;
  g_bandWinY = g_edT + bandY;
  g_bandWinW = cw;
  g_bandWinH = ch;
  present(&c, g_hBand, g_bandWinX, g_bandWinY);
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
  int P = CS(7), H = CS(24), tw, W, maxw;
  int lineH = textLineH(g_measureDC, g_fBody);
  cardShortTitle(t, 220);
  maxw = (g_colW > 0 ? g_colW : CS(240)) - P * 2 - CS(22);
  if (maxw < CS(60)) maxw = CS(60);
  copyEllipsis(g_measureDC, g_fBody, t, maxw, ell, 220);
  tw = textW(g_measureDC, g_fBody, ell);
  W = P + CS(9) + tw + CS(6) + CS(12) + P;
  if (g_colW > 0 && W > g_colW) W = g_colW;
  if (!cvCreate(&c, W, H)) return;
  cvFill(&c, 0, 0, W, H, 255, 253, 248, 226);
  cvRect(&c, 0, 0, W, H, 34, 38, 42, 190);
  cvFill(&c, 0, 0, CS(3), H, 198, 58, 58, 255);              /* 左侧红边 */
  cvFill(&c, P, (H - CS(5)) / 2, CS(5), CS(5), 198, 58, 58, 255); /* 小红点 */
  drawTextAt(&c, c.hdc, P + CS(9), (H - lineH) / 2 + CS(1), ell, g_fBody, 34, 38, 42, 255);
  cvChevron(&c, W - P - CS(10), (H - CS(8)) / 2, CS(4), 0, 105, 111, 118, 255);
  g_cardHeadH = H;
  present(&c, g_hCard, cardPX(), cardPY());
  cvFree(&c);
}

/* 成功结果卡：编译通过 / 运行结束（用户要的“玻璃板上面那块运行结果”） */
static void renderOkCard(void)
{
  Canvas c;
  int W = g_colW > 0 ? g_colW : CS(300), P = CS(9);
  int hH = CS(30);
  int lineHBody = textLineH(g_measureDC, g_fBody);
  int lineHHead = textLineH(g_measureDC, g_fHead);
  int lineHSmall = textLineH(g_measureDC, g_fSmall);
  WCHAR lines[8][1024];
  WCHAR head[220], tips[400];
  int nBody, nTips = 0, i, y, total;

  nBody = wrapLines(g_measureDC, g_fBody, gd.cause, W - P * 2, &lines[0][0], 1024, 3, 240);
  if (nBody < 1) nBody = 1;
  tips[0] = 0;
  if (gd.tips[0] && jsonArrAt(g_glassDoc, "tips", 0, tips, 400) && tips[0]) {
    WCHAR one[300];
    WCHAR merged[400];
    wcsncpy(merged, tips, 380); merged[380] = 0;
    if (jsonArrAt(g_glassDoc, "tips", 1, one, 300) && one[0]) {
      wcsncat(merged, L" · ", 390 - (int)wcslen(merged));
      wcsncat(merged, one, 390 - (int)wcslen(merged));
    }
    wcsncpy(tips, merged, 399); tips[399] = 0;
    nTips = wrapLines(g_measureDC, g_fBody, tips, W - P * 2, &lines[0][0], 1024, 2, 240);
  }

  total = hH + P + nBody * lineHBody + (nTips ? lineHSmall + nTips * lineHBody : 0) + P;
  if (!cvCreate(&c, W, total)) return;

  cvFill(&c, 0, 0, W, total, 255, 253, 248, 244);
  cvRect(&c, 0, 0, W, total, 34, 38, 42, 255);
  cvFill(&c, 1, 1, W - 2, hH - 1, 236, 246, 238, 250);       /* 淡绿标题栏 */
  cvFill(&c, 1, hH - 1, W - 2, CS(2), 34, 38, 42, 255);
  cvFill(&c, 0, 0, CS(3), total, 46, 140, 92, 255);            /* 左侧绿边 */

  {
    const WCHAR *t = gd.title[0] ? gd.title : L"运行结果";
    int hw;
    _snwprintf(head, 200, L"小D · %s", t);
    head[200] = 0;
    hw = textW(c.hdc, g_fHead, head);
    if (hw > W - CS(50)) {
      WCHAR ell[220];
      copyEllipsis(c.hdc, g_fHead, head, W - CS(56), ell, 220);
      drawTextAt(&c, c.hdc, P, (hH - lineHHead) / 2 + CS(2), ell, g_fHead, 30, 80, 52, 255);
    } else {
      drawTextAt(&c, c.hdc, P, (hH - lineHHead) / 2 + CS(2), head, g_fHead, 30, 80, 52, 255);
    }
    cvChevron(&c, W - CS(20), (hH - CS(8)) / 2, CS(4), 1, 105, 111, 118, 255);
  }
  g_cardHeadH = hH;

  y = hH + P;
  for (i = 0; i < nBody; i++) {
    drawTextAt(&c, c.hdc, P, y, lines[i], g_fBody, 34, 38, 42, 255);
    y += lineHBody;
  }
  if (nTips) {
    y += CS(2);
    for (i = 0; i < nTips; i++) {
      drawTextAt(&c, c.hdc, P, y, lines[i], g_fSmall, 76, 96, 84, 255);
      y += lineHBody;
    }
  }
  present(&c, g_hCard, cardPX(), cardPY());
  cvFree(&c);
}

static void renderCard(void)
{
  Canvas c;
  int W = g_colW > 0 ? g_colW : CS(300), P = CS(9);
  int capH = (g_edB - g_edT) - CS(16);
  int hH = CS(30);
  int y, i, n, total;
  WCHAR lines[16][1024];
  int lineHBody, lineHHead, lineHSmall, lineHMono;
  RECT headRect;

  if (!gd.on || !wcscmp(gd.kind, L"busy") || g_cardHidden) { hideWindow(g_hCard); return; }
  if (g_collapsed) { renderCollapsed(); return; }
  if (gd.ok) { renderOkCard(); return; }
  if (capH > CS(320)) capH = CS(320);

  lineHHead = textLineH(g_measureDC, g_fHead);
  lineHBody = textLineH(g_measureDC, g_fBody);
  lineHSmall = textLineH(g_measureDC, g_fSmall);
  lineHMono = textLineH(g_measureDC, g_fMono);

  /* 先测总高度：原因(≤3行) + 改法(≤3行) + 代码(≤4行) + 技巧(≤2行) */
  total = hH + P * 2 + CS(16) + 0;
  total += lineHBody * 3 + lineHSmall;
  total += lineHSmall + lineHBody * 3;
  total += lineHMono * 2 + CS(8);
  total += lineHSmall + lineHBody * 2;

  if (total > capH) { total = capH; }

  if (!cvCreate(&c, W, total)) return;

  /* 卡片底 */
  cvFill(&c, 0, 0, W, total, 255, 253, 248, 244);
  cvRect(&c, 0, 0, W, total, 34, 38, 42, 255);
  /* 标题栏 */
  cvFill(&c, 1, 1, W - 2, hH - 1, 242, 238, 228, 250);
  cvFill(&c, 1, hH - 1, W - 2, CS(2), 34, 38, 42, 255);

  {
    WCHAR head[220];
    int hw;
    if (gd.line > 0)
      wsprintfW(head, L"小D · 第 %d 行", gd.line);
    else
      wcsncpy(head, L"小D · 代码体检", 200);
    head[200] = 0;
    hw = textW(c.hdc, g_fHead, head);
    if (hw > W - CS(50)) {
      WCHAR t[220];
      copyEllipsis(c.hdc, g_fHead, head, W - CS(56), t, 220);
      drawTextAt(&c, c.hdc, P, (hH - lineHHead) / 2 + CS(2), t, g_fHead, 34, 38, 42, 255);
    } else {
      drawTextAt(&c, c.hdc, P, (hH - lineHHead) / 2 + CS(2), head, g_fHead, 34, 38, 42, 255);
    }
    cvChevron(&c, W - CS(20), (hH - CS(8)) / 2, CS(4), 1, 105, 111, 118, 255);
  }

  g_cardHeadH = hH;

  y = hH + P;
  /* ① 错误原因 */
  {
    WCHAR label[80];
    wsprintfW(label, L"① 错误原因");
    if (y + lineHSmall < total) {
      drawTextAt(&c, c.hdc, P, y, label, g_fSmall, 176, 52, 60, 255);
      y += lineHSmall + CS(2);
      n = wrapLines(c.hdc, g_fBody, gd.cause, W - P * 2, &lines[0][0], 1024, 4, 240);
      for (i = 0; i < n; i++) {
        if (y + lineHBody > total - P) break;
        drawTextAt(&c, c.hdc, P, y, lines[i], g_fBody, 34, 38, 42, 255);
        y += lineHBody;
      }
    }
  }

  y += CS(6);
  /* ② 具体改这里 */
  {
    WCHAR label[120];
    WCHAR wline[700];
    wsprintfW(label, L"② 具体改这里");
    if (y + lineHSmall < total) {
      drawTextAt(&c, c.hdc, P, y, label, g_fSmall, 44, 122, 111, 255);
      y += lineHSmall + CS(2);
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
        n = wrapLines(c.hdc, g_fBody, tmp, W - P * 2 - CS(6), &lines[0][0], 1024, 2, 240);
        for (i = 0; i < n && cnt < 2; i++, cnt++) {
          if (y + lineHBody > total - P) break;
          drawTextAt(&c, c.hdc, P + CS(4), y, lines[i], g_fBody, 68, 74, 80, 255);
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
      boxH = cl * lineHMono + CS(8);
      if (y + boxH > total - P) boxH = (total - P - y);
      if (boxH > lineHMono) {
        cvFill(&c, P, y, W - P * 2, boxH, 35, 38, 42, 246);
        for (k = 0; k < cl; k++) {
          int ty = y + CS(4) + k * lineHMono;
          WCHAR ell[600];
          if (ty + lineHMono > y + boxH) break;
          copyEllipsis(c.hdc, g_fMono, codeLines[k], W - P * 2 - CS(10), ell, 600);
          drawTextAt(&c, c.hdc, P + CS(6), ty, ell, g_fMono, 234, 229, 218, 255);
        }
        y += boxH + CS(6);
      }
    }
  }

  /* ③ 知识点 · 技巧 */
  if (y + lineHSmall + lineHBody < total - P) {
    WCHAR label[80], tips[400];
    wsprintfW(label, L"③ 知识点 · 技巧");
    drawTextAt(&c, c.hdc, P, y, label, g_fSmall, 47, 95, 143, 255);
    y += lineHSmall + CS(2);
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

  present(&c, g_hCard, cardPX(), cardPY());
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
    present(&c, g_hStrip, stripPX(), stripPY());
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

  present(&c, g_hStrip, stripPX(), stripPY());
  cvFree(&c);
}

/* 没有错误时：在代码下方的空白处放一条绿色提示，不标行、不弹讲解卡 */
static void renderOkHint(void)
{
  Canvas c;
  int w = g_stripW, h = g_stripH;
  int P = CS(10), x, tw, hHead;
  WCHAR t[200];

  if (w < 40 || h < 20) return;
  if (!cvCreate(&c, w, h)) return;

  cvFill(&c, 0, 0, w, h, 246, 252, 247, 242);
  cvFill(&c, 0, 0, S(4), h, 60, 160, 90, 255);
  cvRect(&c, 0, 0, w, h, 34, 38, 42, 170);

  hHead = textLineH(c.hdc, g_fHead);
  x = P + S(6);
  tw = w - x - P;
  if (tw < S(40)) tw = S(40);

  copyEllipsis(c.hdc, g_fHead, L"代码正常，请运行测试", tw, t, 200);
  drawTextAt(&c, c.hdc, x, P, t, g_fHead, 36, 120, 70, 255);
  copyEllipsis(c.hdc, g_fBody, L"没有发现语法错误，运行看看结果", tw, t, 200);
  drawTextAt(&c, c.hdc, x, P + hHead + CS(3), t, g_fBody, 91, 97, 103, 255);

  present(&c, g_hStrip, stripPX(), stripPY());
  cvFree(&c);
}

/* 提示条的高度（内容） */
static int stripHeight(void)
{
  int lh = textLineH(g_measureDC, g_fBody);
  int lhHead = textLineH(g_measureDC, g_fHead);
  int th = CS(10) + lhHead + CS(3) + lh * 2 + CS(10);
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

/* 编辑器内容的轻量指纹：只用来判断“代码是不是真的变了”。
   一样就说明这一轮测量到的位置抖动是噪声，卡片横向不动。 */
static int inkSigOfText(void)
{
  unsigned h = 5381u;
  int i, step;
  if (g_editTextLen <= 0) return 0;
  step = g_editTextLen / 96;
  if (step < 1) step = 1;
  for (i = 0; i < g_editTextLen; i += step)
    h = h * 33u + (unsigned char)g_editText[i];
  return (int)(h & 0x7fffffff) ^ g_editTextLen;
}

static void layout(void)
{
  int edW = g_edR - g_edL, edH = g_edB - g_edT;
  int line = gd.line;
  int okMode = gd.on && gd.ok && wcscmp(gd.kind, L"busy") != 0;
  g_bandLineY = -1; g_bandLineH = 0; g_bandFullRow = 0;

  if (okMode) {
    /* 没有错误：不标行、不弹讲解卡，只在代码下方的空白处放一条
       “代码正常，请运行测试”，不压住代码。 */
    int hHead = textLineH(g_measureDC, g_fHead);
    int hBody = textLineH(g_measureDC, g_fBody);
    int w1 = textW(g_measureDC, g_fHead, L"代码正常，请运行测试");
    int w2 = textW(g_measureDC, g_fBody, L"没有发现语法错误，运行看看结果");
    int w = (w1 > w2 ? w1 : w2) + CS(30);
    int h = CS(10) + hHead + CS(3) + hBody + CS(10);
    int x, y;
    RECT wa;
    if (w < CS(190)) w = CS(190);
    if (h < CS(46)) h = CS(46);
    if (w > edW - S(20)) w = edW - S(20);
    /* 放在“代码区”里，别压住左边的行号栏 */
    x = g_edL + (g_codeX0 > 0 ? g_codeX0 : S(60)) + S(20);
    y = g_edT + (g_lastCodeY > 0 ? g_lastCodeY : edH / 2) + S(12);
    if (y + h > g_edB - S(10)) y = g_edB - S(10) - h;
    if (y < g_edT + S(8)) y = g_edT + S(8);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    if (x + w > wa.right - S(8)) x = wa.right - S(8) - w;
    if (x < wa.left + S(8)) x = wa.left + S(8);
    g_stripX = x; g_stripY = y; g_stripW = w; g_stripH = h;
    g_cardX = g_stripX; g_cardY = g_stripY;
    return;
  }

  if (line > 0) {
    int rel = line - g_topLine;
    int hh = g_pitch - S(2);
    int y;
    if (hh < S(10)) hh = S(10);
    /* 标行位置 = “第 1 行的行顶在文档里的 y” + (行号-1)×行距 - 已经滚过的像素。
       全部是绝对量，没有任何逐个滚动累加 —— 上滑下滑多少次都只会回到同一个位置。 */
    if (g_baseY > 0 && g_pitch > S(8))
      y = g_baseY + (line - 1) * g_pitch - g_scrollPx;
    else
      y = g_firstTextY + rel * g_pitch;
    /* Dev-C++ 自己给这一行涂了底色（红=错误行、蓝=选中行）时，直接压上去 */
    {
      int ci;
      for (ci = 0; ci < g_colRowN; ci++) {
        if (g_colLine[ci] == line && g_colScroll == g_scrollTop) {
          y = g_colRowTop[ci];
          hh = g_colRowBot[ci] - g_colRowTop[ci] + 1;
          break;
        }
      }
    }
    /* 只剩不到半行露在边上时，就当它已经滚出可视区：
       以前会夹到第一行/最后一行，看起来像“标到了上一行/下一行”。 */
    if (g_pitch > S(8) && y > -hh / 2 && y < edH) {
      if (y < 0) y = 0;
      if (y + hh > edH) { y = edH - hh; if (y < 0) y = 0; }
      g_bandLineY = y;
      g_bandLineH = hh;
    }
  }

  /* 助手浮窗：一个普通的独立浮窗。
     默认贴在 Dev-C++ 窗口右侧、靠上一点；用户拖过或拉过宽度之后，
     位置和宽度就完全按用户的来，不再跟着代码滚动/出错行重排。 */
  {
    int stripH, cardH, colW, totalH;
    RECT wa;
    int x, y;

    stripH = stripHeight();
    if (stripH < CS(40)) stripH = CS(40);
    if (stripH > CS(600)) stripH = CS(600);

    if (g_userW > 0) {
      colW = g_userW;
    } else {
      int avail = edW - CS(60);
      colW = avail > CS(330) ? CS(330) : avail;
      if (colW < CS(240)) colW = CS(240);
    }

    cardH = g_collapsed ? CS(24) : CS(320);
    totalH = stripH + CS(6) + cardH;

    g_colW = colW;
    g_stripW = colW;
    g_stripH = stripH;

    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);

    if (g_userMoved) {
      /* 用户自己拖过：位置固定，不再自动贴代码 */
      g_stripX = g_cardX;
      g_stripY = g_cardY - stripH - S(6);
    } else {
      /* 代码浮窗跟着“标红的错行”走：贴在错行右手边的空白处。
         横向基准只在「代码文本真的变了」时重算一次，平时一律锁死 ——
         测量每帧都会抖几像素，跟着抖就是“乱飘”。 */
      int bandTop = (g_bandLineY >= 0) ? (g_edT + g_bandLineY) : (g_edT + S(60));
      int ink;
      int xMax;
      int sig = inkSigOfText();
      static DWORD inkChk = 0;
      if (g_cardInk <= 0 || sig != g_inkSig) {
        int t = textInkRight();
        if (t > 0) { g_cardInk = t; g_inkSig = sig; }
      } else if (GetTickCount() - inkChk > 1200) {
        int t;
        inkChk = GetTickCount();
        t = textInkRight();
        if (t > 0 && abs(t - g_cardInk) > S(120)) g_cardInk = t;  /* 换了字号之类的兜底 */
      }
      ink = g_cardInk;
      if (ink <= 0) ink = edW / 2;
      if (ink > edW) ink = edW;
      x = g_edL + ink + S(20);
      y = bandTop - CS(10);

      /* 最多靠到编辑区右边，再往右就越出代码区了 */
      xMax = g_edR - S(10) - colW;
      if (xMax < g_edL + S(10)) xMax = g_edL + S(10);
      if (x > xMax) x = xMax;

      /* 不跟助手浮窗重合：它盖住的那段右边空白让出来 */
      if (g_panelL > g_edL && g_panelB > g_edT && g_panelT < g_edB) {
        int lim = g_panelL - S(12) - colW;
        if (x > lim) x = lim;
        if (x < g_edL + S(10)) x = g_edL + S(10);
      }
      if (x + colW > wa.right - S(8)) x = wa.right - S(8) - colW;
      if (x < wa.left + S(8)) x = wa.left + S(8);
      /* 纵向：默认贴着错行往下排；下面放不下就整块挪到错行上面，
         还是紧挨着这一行，而不是被夹到编辑区底部、看起来“飞到上面去了”。 */
      if (y + totalH <= g_edB - S(8)) {
        /* 贴在错行右侧往下排，正好 */
      } else if (y - totalH >= g_edT + S(8)) {
        y = y - totalH;            /* 放上面：整块底边贴着错行 */
      } else {
        y = g_edB - S(8) - totalH; /* 上面下面都挤不下：贴着编辑区底边 */
      }
      if (y < g_edT + S(8)) y = g_edT + S(8);
      if (y + totalH > wa.bottom - S(12)) y = wa.bottom - S(12) - totalH;
      if (y < wa.top + S(8)) y = wa.top + S(8);
      g_stripX = x;
      g_stripY = y;
    }

    if (g_stripY < wa.top) g_stripY = wa.top;
    if (g_stripY + totalH > wa.bottom) g_stripY = wa.bottom - totalH;
    if (g_stripX + colW > wa.right) g_stripX = wa.right - colW;
    if (g_stripX < wa.left) g_stripX = wa.left;

    g_cardX = g_stripX;
    g_cardY = g_stripY + stripH + S(6);
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

/* Dev-C++ 正在运行程序时标题会带 [Executing]：这时控制台窗口会盖住编辑区，
   不能再去抓屏测量（抓到的全是控制台画面），但玻璃要留在原地继续显示 */
static int hostExecuting(void)
{
  WCHAR t[256];
  if (!g_hHost || !IsWindow(g_hHost)) return 0;
  if (!GetWindowTextW(g_hHost, t, 256)) return 0;
  return wcsstr(t, L"[Executing]") != NULL;
}

/* 控制台窗口（Windows 10 老控制台 / Windows Terminal） */
static int isConsoleWindow(HWND h)
{
  WCHAR cls[128];
  if (!h) return 0;
  if (!GetClassNameW(h, cls, 128)) return 0;
  if (!_wcsicmp(cls, L"ConsoleWindowClass")) return 1;
  if (!_wcsicmp(cls, L"CASCADIA_HOSTING_WINDOW_CLASS")) return 1;
  if (wcsstr(cls, L"PseudoConsole")) return 1;
  return 0;
}

static void initFonts(void)
{
  if (g_fHead) return;
  g_fHead = CreateFontW(-CS(14), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                        OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
  g_fBody = CreateFontW(-CS(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                        OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
  g_fSmall = CreateFontW(-CS(11), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
  g_fMono = CreateFontW(-CS(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                        OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                        FIXED_PITCH | FF_MODERN, L"Consolas");
  g_fTag = CreateFontW(-CS(11), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
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
  int lastTop = -999, lastLine = -999, lastDrawn = 0, lastScrollPos = -99999;
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
  /* 三个窗口都从屏幕截图里排除掉：抓屏测量时就不用再藏一遍，
     彻底消掉“每隔一会儿整个浮层闪一下”的现象 */
  {
    WCHAR dbg[MAX_PATH];
    int a, b, c;
    /* 调试用：state\showcapture.txt 存在时不做排除，方便截图核对标行 */
    wsprintfW(dbg, L"%s\\state\\showcapture.txt", gHome);
    if (GetFileAttributesW(dbg) != INVALID_FILE_ATTRIBUTES) {
      g_capExclOk = 0;
      glog(L"capExcl=0 (debug)");
    } else {
      a = excludeFromCapture(g_hBand);
      b = excludeFromCapture(g_hStrip);
      c = excludeFromCapture(g_hCard);
      g_capExclOk = (a && b && c);
      glog(L"capExcl=%d (%d,%d,%d)", g_capExclOk, a, b, c);
    }
  }
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
            /* 新的一次分析也保持展开：代码浮窗的讲解默认不折叠 */
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
      BOOL executing = hostExecuting();
      /* Dev-C++ 主窗自己被藏起来（比如程序正在 [Executing]）时绝不能画：
         否则玻璃会浮在别的程序上面 */
      if (fg && g_hHost) {
        WCHAR t[256], cls[128];
        active = (fg == g_hHost || GetAncestor(fg, GA_ROOT) == g_hHost);
        if (!active && g_hEdit && GetAncestor(g_hEdit, GA_ROOT) == fg) active = 1;
        /* 7.x：居前的是外层 wrapper，编辑器在另一个 top-level form 里，
           两者属于同一个进程 —— 按进程认，别因为窗口不是同一个就把玻璃藏了 */
        if (!active) {
          DWORD p1 = 0, p2 = 0;
          GetWindowThreadProcessId(fg, &p1);
          GetWindowThreadProcessId(g_hHost, &p2);
          if (p1 && p1 == p2) active = 1;
        }
        if (!active) {
          GetClassNameW(fg, cls, 128);
          if (!wcscmp(cls, CLASS_GLASS)) active = 1;
          else {
            GetWindowTextW(fg, t, 256);
            if (wcsstr(t, L"DevAI")) active = 1;
            /* 系统/后台服务的隐藏窗口抢焦点时不当成“切走了”，免得玻璃乱闪 */
            else if (!IsWindowVisible(fg)) active = 1;
            /* 运行程序时控制台会抢到前台：玻璃要留在编辑区继续显示，
               不然一运行就全没了（用户会以为“运行界面不显示”） */
            else if (executing && isConsoleWindow(fg)) active = 1;
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
    readTopLine();                 /* 更新 g_scrollTop / g_scrollTopLine */
    int top = topLineNow();        /* 锚点相对推算出的第一可见行 */
    int changedGeo = (edL != lastEdL || edT != lastEdT || edR != lastEdR || edB != lastEdB || top != lastTop);

    if (!gd.hasData || !gd.on) {
      hideAll();
      lastDrawn = 0;
      writeAlive();
      lastEdL = edL; lastTop = top; lastScrollPos = g_scrollTop; lastLine = gd.line;
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

    /* 编译/运行的事件丢了（比如编译器没写结束事件、AI 超时），
       “正在编译”不能一直挂在代码上挡着 */
    if (!wcscmp(gd.kind, L"busy") && gd.ts != 0 &&
        (DWORD)(GetTickCount() - gd.ts) > 25000) {
      hideAll();
      lastDrawn = 0;
      writeAlive();
      Sleep(300);
      continue;
    }

    {
      /* 抓屏必须先把玻璃自己藏 70ms（否则会把自己的高亮抓进去），
         所以“重新量”一定要省。窗口被拖动/移动时内容并没有变，
         只要把浮层跟过去就行；只有尺寸变了、滚动过、有输入才重新量。
         以前 changedGeo 也算重新量，拖窗口就变成每帧藏一次 —— 满屏闪。 */
      int sizeChanged = ((edR - edL) != (lastEdR - lastEdL)) ||
                        ((edB - edT) != (lastEdB - lastEdT));
      /* 滚动可能不到一整行（7.x 是像素滚动）：只要滚动条动过就算，色带才能一个像素一个像素地跟 */
      int scrolled = (top != lastTop) || (g_scrollTop != lastScrollPos);
      /* 运行中的程序会把控制台窗口盖在编辑区上，这时抓屏量到的是控制台画面，
         行距/右边界全都会算错 —— 运行期间只沿用上一次量好的几何 */
      int executing = hostExecuting();
      if (!executing && (sizeChanged || g_forceMeasure || (g_needDraw && g_pitch <= 1))) {
        int pitchBefore = g_pitch;
        /* 只有窗口尺寸变了、或者第一次测量，才允许改行距；
           单纯滚动/选中行变色引起的重测一律沿用已经量准的行距 */
        g_measureTrust = (sizeChanged || g_pitch <= S(8)) ? 1 : 0;
        g_forceMeasure = 0;
        readTopLine();
        {
          int scrollBeforeCap = g_scrollTop;
          captureEditor();
          readTopLine();   /* 抓屏这一刻的滚动位置：和抓屏前不一样就说明画面可能错开一帧 */
          g_measScrollStable = (g_scrollTop == scrollBeforeCap) ? 1 : 0;
        }
        measureLines();   /* 抓空了会在里面重新置 g_forceMeasure，下一轮再试 */
        if (g_capFallback) { g_needDraw = 1; g_capFallback = 0; }
        readTopLine();
        /* 只有在“绝对换算可信”的时候才重设锚点：第一次、尺寸变了、行距变了。
           平时滚动全靠锚点相对推算，不会因为滚动条 nPos 的绝对语义差一位。
           画面拟合出来的第一可见行和锚点推算值对不上（老版滚动条僵死）时，
           以画面为准重新锚定，否则色带会一直错下去。 */
        {
          int reanchor = (!g_ancValid || sizeChanged ||
                          (pitchBefore > S(8) && g_pitch != pitchBefore));
          if (g_fitTop > 0 && g_fitTop != topLineNow()) reanchor = 1;
          if (reanchor)
            anchorScrollAt(g_fitTop > 0 ? g_fitTop : g_scrollTopLine);
        }
        g_colScroll = g_scrollTop;   /* 变色行的 y 只在这个滚动位置有效 */
        top = topLineNow();
        g_topLine = top;
        g_needDraw = 1;
        /* 窗口挪过位置就一定要重画：否则浮层还停在旧坐标上（色带“消失”就是这个） */
        lastEdL = edL; lastEdT = edT; lastEdR = edR; lastEdB = edB;
        lastTop = top; lastScrollPos = g_scrollTop;
      } else if (scrolled || changedGeo) {
        /* 滚动：不抓屏、不重测 —— 只把“第一可见行”按滚动条位移换算一下。
           行距/相位已经量准，色带位置是纯算术，所以每一帧都能立刻跟上滚轮。 */
        g_topLine = top;
        lastEdL = edL; lastEdT = edT; lastEdR = edR; lastEdB = edB;
        lastTop = top; lastScrollPos = g_scrollTop;
        g_needDraw = 1;
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
    }

    layout();
    /* 位置平滑跟随：目标位置（g_cardY/g_stripY…）和实际画出去的位置不一样时，
       每帧靠近 1/3，看起来是滑过去而不是瞬移。只挪位置，不重新算内容。
       跳得太远（换了错行/刚测量出真实行）就直接到位，否则会看到它从上一个位置
       一路滑过去，看起来就是“先飞到上面再飘下来”。 */
    {
      if (!g_curValid || g_dragging || g_resizing ||
          abs(g_cardY - g_curCardY) > S(160) || abs(g_stripY - g_curStripY) > S(160) ||
          abs(g_cardX - g_curCardX) > S(200) || abs(g_stripX - g_curStripX) > S(200)) {
        g_curCardX = g_cardX; g_curCardY = g_cardY;
        g_curStripX = g_stripX; g_curStripY = g_stripY;
        g_curValid = 1;
      }
      {
        int d, s;
        int nx = g_curCardX, ny = g_curCardY, sx = g_curStripX, sy = g_curStripY;
        d = g_cardX - nx; if (d) { s = d / 3; if (s > -2 && s < 2) s = d > 0 ? 2 : -2; nx += s; }
        d = g_cardY - ny; if (d) { s = d / 3; if (s > -2 && s < 2) s = d > 0 ? 2 : -2; ny += s; }
        d = g_stripX - sx; if (d) { s = d / 3; if (s > -2 && s < 2) s = d > 0 ? 2 : -2; sx += s; }
        d = g_stripY - sy; if (d) { s = d / 3; if (s > -2 && s < 2) s = d > 0 ? 2 : -2; sy += s; }
        if (nx != g_curCardX || ny != g_curCardY || sx != g_curStripX || sy != g_curStripY) {
          if (abs(g_cardX - nx) <= 2 && abs(g_cardY - ny) <= 2) { nx = g_cardX; ny = g_cardY; }
          if (abs(g_stripX - sx) <= 2 && abs(g_stripY - sy) <= 2) { sx = g_stripX; sy = g_stripY; }
          g_curCardX = nx; g_curCardY = ny;
          g_curStripX = sx; g_curStripY = sy;
          g_needDraw = 1;   /* 用新位置重画一次（内容不变） */
        }
      }
    }
    /* 规矩：当前画面上真的标出了错误行，才显示代码浮窗（提示条 + 讲解卡）。
       标行不在可视区（滚走了/没行号）就一律收起，别浮在代码上碍事。 */
    {
      int okMode = gd.ok && wcscmp(gd.kind, L"busy") != 0;
      int bandOk = (gd.line > 0 && g_bandLineY >= 0 && !hostExecuting());
      g_bandVisible = okMode ? 0 : bandOk;
    }
    /* !lastDrawn：窗口被收起过（切走/没找到 Dev-C++）就必须重画，
       否则 SetWindowPos 不会把隐藏窗口显示出来，浮层会一直不出现 */
    if (g_needDraw || !lastDrawn || gd.line != lastLine ||
        g_bandLineY != g_sigBandY || g_bandLineH != g_sigBandH ||
        g_bandVisible != g_sigBandVis ||
        g_cardX != g_sigCardX || g_cardY != g_sigCardY ||
        g_stripX != g_sigStripX || g_stripY != g_sigStripY ||
        g_collapsed != g_sigCollapsed) {
      if (!wcscmp(gd.kind, L"busy")) {
        hideWindow(g_hBand);
        hideWindow(g_hCard);
        renderStrip();
      } else if (gd.ok) {
        /* 没有错误：只在代码空白处提示“代码正常，请运行测试” */
        hideWindow(g_hBand);
        hideWindow(g_hCard);
        renderOkHint();
      } else if (g_bandVisible) {
        renderBand();
        renderCard();
        renderStrip();
      } else {
        /* 没有标行就不显示代码浮窗 */
        hideWindow(g_hBand);
        hideWindow(g_hCard);
        hideWindow(g_hStrip);
      }
      g_needDraw = 0;
      g_sigLine = gd.line;
      g_sigBandY = g_bandLineY;
      g_sigBandH = g_bandLineH;
      g_sigBandVis = g_bandVisible;
      g_sigCardX = g_cardX;
      g_sigCardY = g_cardY;
      g_sigStripX = g_stripX;
      g_sigStripY = g_stripY;
      g_sigCollapsed = g_collapsed;
      lastLine = gd.line;
      lastTop = top;
      lastDrawn = 1;
    } else if (lastDrawn) {
      /* 只跟随窗口移动：色带按相对编辑区的偏移跟过去 */
      moveIfMoved(g_hBand, g_edL + g_bandRelX, g_edT + g_bandRelY);
      moveIfMoved(g_hStrip, g_stripX, g_stripY);
      moveIfMoved(g_hCard, g_cardX, g_cardY);
    }

    writeAlive();
    /* 有行要标的时候高频轮询滚动条：滚轮一动，色带就跟着走 */
    if (gd.on && gd.line > 0) Sleep(15);
    else Sleep(200);
  }
  return 0;
}
