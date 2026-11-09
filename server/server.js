'use strict';
/*
 * DevAI 助手服务（零依赖 Node）
 * - 监听 D:\devai\queue 里由拦截器写入的事件
 * - 解析 gcc/g++ 报错、程序崩溃退出码
 * - 调用 OpenAI 兼容 API，把错误翻译成小白能懂的讲解
 * - 通过 SSE 把实时日志 / 诊断结果推给助手面板（Edge app 窗口）
 */

const http = require('http');
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

const HOME = path.resolve(__dirname, '..');
const QUEUE = path.join(HOME, 'queue');
const STATE_DIR = path.join(HOME, 'state');
const CONFIG_FILE = path.join(HOME, 'config.json');
const PORT_FILE = path.join(HOME, 'port.txt');
const PID_FILE = path.join(HOME, 'agent.pid');
const PROCESSED_FILE = path.join(HOME, 'state', 'processed.json');
const UI_DIR = path.join(__dirname, 'ui');

const DEFAULT_CONFIG = {
  apiBase: 'https://api.deepseek.com',
  apiKey: '',
  model: 'deepseek-chat',
  temperature: 0.2,
  maxTokens: 700,
  port: 47901,
  autoOpenPanel: true,
  explainRuntime: true,
  explainWarnings: false
};

/* ------------------------------------------------------------------ 基础工具 */

function ensureDirs() {
  for (const d of [QUEUE, STATE_DIR]) {
    try { fs.mkdirSync(d, { recursive: true }); } catch (e) {}
  }
}

function loadConfig() {
  try {
    const raw = fs.readFileSync(CONFIG_FILE, 'utf8').replace(/^\uFEFF/, '');
    return Object.assign({}, DEFAULT_CONFIG, JSON.parse(raw));
  } catch (e) {
    return Object.assign({}, DEFAULT_CONFIG);
  }
}

function saveConfig(patch) {
  const cfg = Object.assign(loadConfig(), patch || {});
  fs.writeFileSync(CONFIG_FILE, JSON.stringify(cfg, null, 2), 'utf8');
  return cfg;
}

/* 源码/日志编码兜底：优先 UTF-8，出现乱码再按 GBK 解 */
function smartRead(file) {
  try {
    const buf = fs.readFileSync(file);
    let text = buf.toString('utf8');
    if (text.indexOf('\uFFFD') >= 0) {
      try { text = new TextDecoder('gbk').decode(buf); } catch (e) {}
    }
    return text;
  } catch (e) {
    return '';
  }
}

function kvParse(text) {
  const obj = { args: [] };
  for (const line of text.split(/\r?\n/)) {
    if (!line) continue;
    const i = line.indexOf('=');
    if (i < 0) continue;
    const k = line.slice(0, i);
    const v = line.slice(i + 1);
    if (k === 'arg') obj.args.push(v);
    else obj[k] = v;
  }
  return obj;
}

const EXIT_CODES = {
  3221225477: { hex: '0xC0000005', name: '访问冲突（Access Violation）', hint: '通常是把数组下标写越界了，或者用了空指针 / 野指针（比如没初始化的指针）' },
  3221225725: { hex: '0xC00000FD', name: '栈溢出（Stack Overflow）', hint: '常见于递归没有终止条件，或者在函数里开了特别大的局部数组' },
  3221225620: { hex: '0xC0000094', name: '整数除以零', hint: '某次除法 / 取模的除数是 0，比如 a / b 而 b 恰好是 0' },
  3221226505: { hex: '0xC0000409', name: '栈缓冲区被破坏', hint: '数组越界写坏了栈内存，或者程序主动调用了 abort() / 抛出了未捕获异常' },
  3221225595: { hex: '0xC0000095', name: '整数溢出', hint: 'int 装不下计算结果了，或者负数开方等非法运算' },
  3221225786: { hex: '0xC000013A', name: '被强制中断', hint: '程序还在运行时窗口被关闭或按了 Ctrl+C' },
  3221226356: { hex: '0xC0000374', name: '堆内存损坏', hint: 'delete / free 了不该释放的内存，或重复释放、越界写堆' },
  3: { hex: '0x3', name: '程序中止（abort）', hint: '可能触发了 assert 断言失败、未捕获的异常，或 std::terminate' },
  255: { hex: '0xFF', name: '异常退出', hint: '程序自己 return 了非 0，或者运行时库报了错' }
};

function describeExit(code) {
  if (code === 0) return { name: '正常结束', hex: '0x0', hint: '' };
  const hit = EXIT_CODES[code];
  if (hit && hit.name) return hit;
  return { hex: '0x' + (code >>> 0).toString(16).toUpperCase(), name: '异常退出', hint: '退出码不是 0，说明程序中途出了问题' };
}

/* -------------------------------------------------------------- gcc 报错解析 */

function isSourceFile(p) {
  return /\.(c|cc|cpp|cxx|cp|c\+\+)$/i.test(p || '');
}

function parseDiagnostics(log) {
  const text = log || '';
  const diags = [];
  const rePath = /^(?:[A-Za-z]:)?[^:\r\n]+$/;
  const re = /^(.+?):(\d+)(?::(\d+))?\s*:\s*(?:fatal error|error|warning|note|致命错误|错误|警告|附注|注意)\s*[:：]\s*(.*)$/i;

  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trimEnd();
    const m = line.match(re);
    if (m && rePath.test(m[1].trim())) {
      const lvlRaw = (line.match(/(?:fatal error|error|warning|note|致命错误|错误|警告|附注|注意)/i) || [''])[0].toLowerCase();
      let level = 'error';
      if (/warning|警告/.test(lvlRaw)) level = 'warning';
      else if (/note|附注|注意/.test(lvlRaw)) level = 'note';
      diags.push({
        file: m[1].trim(),
        line: parseInt(m[2], 10),
        col: m[3] ? parseInt(m[3], 10) : null,
        level,
        message: m[4].trim()
      });
      continue;
    }
    if (/undefined reference to|ld returned \d+ exit status|collect2|multiple definition/i.test(line)) {
      diags.push({ file: '', line: null, col: null, level: 'error', message: line.trim() });
    }
  }

  const errors = diags.filter(d => d.level === 'error');
  const warnings = diags.filter(d => d.level === 'warning');
  return { diags, errors, warnings };
}

/* 取出报错行附近的代码，喂给模型判断 */
function codeContext(file, line, span) {
  const text = smartRead(file);
  if (!text) return '';
  const lines = text.split(/\r?\n/);
  const from = Math.max(1, (line || 1) - (span || 4));
  const to = Math.min(lines.length, (line || 1) + (span || 4));
  const out = [];
  for (let i = from; i <= to; i++) out.push(String(i).padStart(4, ' ') + ' | ' + lines[i - 1]);
  return out.join('\n');
}

function numberedSource(file, limitLines) {
  const text = smartRead(file);
  if (!text) return '';
  const lines = text.split(/\r?\n/);
  const n = Math.min(lines.length, limitLines || 140);
  const out = [];
  for (let i = 0; i < n; i++) out.push(String(i + 1).padStart(4, ' ') + ' | ' + lines[i]);
  if (lines.length > n) out.push('...... (后面的代码省略)');
  return out.join('\n');
}

/* ---------------------------------------------------------------- AI 调用 */

/* 精简提示词：字段短、字数少，总输出 ≤300 字，省 token */
const SYS_DIAG = [
  '你是小D，C/C++ 零基础学生的助教，说话像耐心的学长。',
  '只输出一个 JSON 对象，不要任何多余文字。字段：',
  't 标题≤16字；c 错误原因≤50字，用生活化比喻；w 具体位置≤30字（第几行、改什么）；',
  'h 改法步骤数组，1~3 条，每条≤30字；code 改后可直接替换的关键代码（≤12行）；',
  'k 知识点数组≤3条，每条≤10字；tip 一句话技巧≤30字；l 出错行号；',
  'bad 原来那一行代码；good 改好后那一行；lv 只能是 error/warning/crash/info。',
  '不编造行号；信息不足时给最可能的 1 个原因。总输出≤300字。'
].join('\n');

const SYS_COMPILE = SYS_DIAG;
const SYS_RUNTIME = SYS_DIAG;
const SYS_REVIEW = SYS_DIAG;

const SYS_CHAT = [
  '你是小D，C/C++ 零基础学生的助教。',
  '用口语化中文回答，≤150字，必要时代码不超过 8 行；只答这次问的点，不要重复完整讲解。只输出纯文本。'
].join('\n');

function chatUrl(base) {
  let b = (base || '').trim().replace(/\/+$/, '');
  if (!b) b = DEFAULT_CONFIG.apiBase;
  if (/\/chat\/completions$/.test(b)) return b;
  if (/\/v1$/.test(b)) return b + '/chat/completions';
  if (/deepseek\.com$/.test(b)) return b + '/chat/completions';
  return b + '/v1/chat/completions';
}

async function callAI(messages, opts) {
  const cfg = loadConfig();
  if (!cfg.apiKey) {
    const e = new Error('还没有配置 API Key');
    e.code = 'NO_KEY';
    throw e;
  }
  const body = {
    model: cfg.model || 'deepseek-chat',
    messages,
    temperature: typeof cfg.temperature === 'number' ? cfg.temperature : 0.2,
    max_tokens: cfg.maxTokens || 700,
    stream: false
  };
  if (opts && opts.json !== false) body.response_format = { type: 'json_object' };

  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), (opts && opts.timeout) || 75000);

  async function once(payload) {
    const res = await fetch(chatUrl(cfg.apiBase), {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', Authorization: 'Bearer ' + cfg.apiKey },
      body: JSON.stringify(payload),
      signal: controller.signal
    });
    const text = await res.text();
    if (!res.ok) {
      const err = new Error('AI 接口返回 ' + res.status + '：' + text.slice(0, 300));
      err.status = res.status;
      throw err;
    }
    let data;
    try { data = JSON.parse(text); } catch (e) { throw new Error('AI 接口返回了非 JSON 内容：' + text.slice(0, 200)); }
    if (!data.choices || !data.choices[0]) throw new Error('AI 返回结构异常：' + text.slice(0, 200));
    return data.choices[0].message.content || '';
  }

  try {
    return await once(body);
  } catch (e) {
    /* 有的中转服务不支持 response_format，去掉重试一次 */
    if (body.response_format && e.status && e.status >= 400 && e.status < 500) {
      const b2 = Object.assign({}, body);
      delete b2.response_format;
      return await once(b2);
    }
    throw e;
  } finally {
    clearTimeout(timer);
  }
}

function extractJSON(text) {
  if (!text) return null;
  let s = text.trim().replace(/^```(?:json)?/i, '').replace(/```$/, '').trim();
  try { return JSON.parse(s); } catch (e) {}
  const a = s.indexOf('{');
  const b = s.lastIndexOf('}');
  if (a >= 0 && b > a) {
    try { return JSON.parse(s.slice(a, b + 1)); } catch (e) {}
  }
  return null;
}

function normalizeAdvice(obj, raw) {
  if (!obj) {
    return {
      title: 'AI 的回答',
      explain: raw || '（没有拿到内容）',
      why: '',
      fixSteps: [],
      fixedCode: '',
      concepts: [],
      tip: '',
      level: 'info'
    };
  }
  return {
    title: obj.t || obj.title || '问题分析',
    explain: obj.c || obj.explain || '',
    why: obj.why || '',
    where: obj.w || '',
    fixSteps: Array.isArray(obj.h) ? obj.h : (Array.isArray(obj.fixSteps) ? obj.fixSteps : []),
    fixedCode: obj.code || obj.fixedCode || '',
    concepts: Array.isArray(obj.k) ? obj.k : (Array.isArray(obj.concepts) ? obj.concepts : []),
    tip: obj.tip || '',
    level: obj.lv || obj.level || 'info',
    line: Number.isFinite(+obj.l) && +obj.l > 0 ? +obj.l : (Number.isFinite(+obj.line) && +obj.line > 0 ? +obj.line : null),
    badLine: typeof obj.bad === 'string' ? obj.bad : (typeof obj.badLine === 'string' ? obj.badLine : ''),
    goodLine: typeof obj.good === 'string' ? obj.good : (typeof obj.goodLine === 'string' ? obj.goodLine : '')
  };
}

/* 给面板用的"出错行上下文"：行号 + 附近几行源码，供虚拟形象指认 */
function buildFocus(file, line, col, span) {
  const lines = sourceLines(file);
  if (!file || !lines.length || !line || line < 1 || line > lines.length) return null;
  const pad = span || 3;
  const from = Math.max(1, line - pad);
  const to = Math.min(lines.length, line + pad);
  const win = [];
  for (let i = from; i <= to; i++) win.push({ n: i, t: String(lines[i - 1]).replace(/\t/g, '    ').slice(0, 200) });
  return {
    file, name: path.basename(file), line, col: col || null,
    from, to, total: lines.length, lines: win,
    text: String(lines[line - 1] || '').trim().slice(0, 200)
  };
}

function sourceLines(file) {
  const text = smartRead(file);
  return text ? text.split(/\r?\n/) : [];
}

/* ------------------------------------------------------------ 事件与状态 */

const feed = [];           /* 面板上显示的所有卡片 */
const clients = new Set(); /* SSE 连接 */
const state = {
  phase: 'idle',
  source: '',
  exe: '',
  compiler: '',
  lastOk: null,
  lastErrors: 0
};
let lastScenario = null;   /* 最近一次诊断上下文，用于"重新分析" */
let chatHistory = [];
let lastAnalysisKey = '';  /* 同一错误短时间内不重复调用 AI，省 token */
let lastGlassSpawn = 0;

function pushItem(item) {
  feed.push(item);
  if (feed.length > 30) feed.shift();
  broadcast({ t: 'item', item });
}

/* 只保留最近一条分析，feed 里不再堆历史卡片 */
function pushAnalysis(item) {
  for (let i = feed.length - 1; i >= 0; i--) {
    if (feed[i].type === 'analysis') feed.splice(i, 1);
  }
  broadcast({ t: 'reset' });
  pushItem(item);
}

function updateItem(item) {
  broadcast({ t: 'update', item });
  const idx = feed.findIndex(x => x.id === item.id);
  if (idx >= 0) feed[idx] = item;
}

/* ------------------------------------------------------- 玻璃覆盖层数据 */

/* 把讲解写进 state/glass.json，由 devai_glass.exe 画在 Dev-C++ 界面上 */
function writeGlass(payload) {
  try {
    fs.mkdirSync(STATE_DIR, { recursive: true });
    fs.writeFileSync(path.join(STATE_DIR, 'glass.json'),
      JSON.stringify(Object.assign({ ts: Date.now() }, payload)), 'utf8');
    ensureGlass();
  } catch (e) {}
}

/* 玻璃层进程还活着就不重复拉起 */
function ensureGlass() {
  try {
    let fresh = false;
    try { fresh = Date.now() - fs.statSync(path.join(STATE_DIR, 'glass.alive')).mtimeMs < 5000; } catch (e) {}
    if (fresh || Date.now() - lastGlassSpawn < 8000) return;
    const exe = path.join(HOME, 'bin', 'devai_glass.exe');
    if (!fs.existsSync(exe)) return;
    lastGlassSpawn = Date.now();
    spawn(exe, [], { detached: true, stdio: 'ignore', windowsHide: true }).unref();
  } catch (e) {}
}

function glassFromAdvice(item, advice, extra) {
  const focus = item.focus || {};
  writeGlass(Object.assign({
    on: true,
    kind: 'result',
    ok: false,
    level: advice.level || item.scenario || 'error',
    title: advice.title || '',
    line: advice.line || focus.line || 0,
    file: item.file || focus.file || state.source || '',
    name: focus.name || (item.file ? path.basename(item.file) : ''),
    cause: advice.explain || '',
    where: advice.where || (focus.line ? '第 ' + focus.line + ' 行' : ''),
    how: advice.fixSteps || [],
    code: advice.fixedCode || '',
    tips: advice.concepts || [],
    tip: advice.tip || '',
    bad: advice.badLine || '',
    good: advice.goodLine || ''
  }, extra || {}));
}

function broadcast(obj) {
  const payload = 'data: ' + JSON.stringify(obj) + '\n\n';
  for (const res of clients) {
    try { res.write(payload); } catch (e) {}
  }
}

function broadcastStatus() {
  broadcast({ t: 'status', state });
}

function newId() {
  return Date.now().toString(36) + Math.random().toString(36).slice(2, 7);
}

/* --------------------------------------------------------------- 分析流程 */

async function analyzeCompile(evt, logText, parsed) {
  const srcFile = (evt.args || []).find(isSourceFile) || '';
  lastScenario = { kind: 'compile', srcFile, logText, parsed, evt };

  const primary = parsed.errors[0] || {};
  const key = (primary.message || '') + '|' + (primary.line || 0) + '|' + srcFile;
  /* 同一错误重复编译：不再花 token，沿用面板上已有分析 */
  if (key === lastAnalysisKey && feed.some(x => x.type === 'analysis' && x.status === 'done')) {
    broadcastStatus();
    return;
  }
  lastAnalysisKey = key;

  const id = newId();
  const item = {
    id, type: 'analysis', scenario: 'compile', status: 'loading', ts: Date.now(),
    title: '诊断中…', data: null, error: null,
    focus: buildFocus(srcFile, primary.line, primary.col, 2)
  };
  pushAnalysis(item);

  const userContent = [
    '场景：Dev-C++ 编译失败（' + (evt.compiler === 'gpp' ? 'g++' : 'gcc') + '）',
    '编译器输出（截断）：',
    (logText || '').slice(0, 1200) || '(无输出)',
    primary.message ? '首个错误：第 ' + (primary.line || '?') + ' 行 ' + primary.message : '',
    srcFile ? '源文件 ' + path.basename(srcFile) + '：\n' + numberedSource(srcFile, 120) : '',
    primary.line && srcFile ? '出错行附近：\n' + codeContext(srcFile, primary.line, 2) : ''
  ].filter(Boolean).join('\n');

  try {
    const raw = await callAI([
      { role: 'system', content: SYS_COMPILE },
      { role: 'user', content: userContent }
    ]);
    const advice = normalizeAdvice(extractJSON(raw), raw);
    if (!advice.badLine && item.focus && item.focus.text) advice.badLine = item.focus.text;
    if (!item.focus && advice.line) item.focus = buildFocus(srcFile, advice.line, null, 2);
    Object.assign(item, { status: 'done', title: advice.title, data: advice });
    updateItem(item);
    glassFromAdvice(item, advice, { scenario: 'compile' });
    state.lastAnalysis = { title: advice.title, line: advice.line || (item.focus && item.focus.line) || 0, level: advice.level, ts: Date.now() };
    broadcastStatus();
  } catch (e) {
    Object.assign(item, {
      status: 'error',
      title: '诊断失败',
      error: e.code === 'NO_KEY' ? '还没有填写 API Key，点右上角"设置"填一下就能用了。' : e.message
    });
    updateItem(item);
  }
}

async function analyzeRuntime(evt) {
  const code = parseInt(evt.exit, 10) || 0;
  let srcFile = evt.src || state.source || '';
  if (!srcFile && state.exe) {
    const base = state.exe.replace(/.exe$/i, '');
    for (const ext of ['.cpp', '.c']) {
      if (fs.existsSync(base + ext)) { srcFile = base + ext; break; }
    }
  }
  const info = describeExit(code);
  lastScenario = { kind: 'runtime', srcFile, code, evt };

  const id = newId();
  const item = {
    id, type: 'analysis', scenario: 'runtime', status: 'loading', ts: Date.now(),
    title: '诊断中…', exitCode: code, exitInfo: info, data: null, error: null
  };
  pushAnalysis(item);

  const userContent = [
    '场景：程序编译成功但运行崩溃，退出码 ' + code + '（' + info.hex + '）' + info.name,
    '通常含义：' + info.hint,
    srcFile ? '源代码 ' + path.basename(srcFile) + '：\n' + numberedSource(srcFile, 140) : '没找到源码，只按退出码给排查方向。'
  ].join('\n');

  try {
    const raw = await callAI([
      { role: 'system', content: SYS_RUNTIME },
      { role: 'user', content: userContent }
    ]);
    const advice = normalizeAdvice(extractJSON(raw), raw);
    if (srcFile && advice.line && !item.focus) item.focus = buildFocus(srcFile, advice.line, null, 2);
    if (item.focus && !advice.badLine) advice.badLine = item.focus.text;
    Object.assign(item, { status: 'done', title: advice.title, data: advice });
    updateItem(item);
    glassFromAdvice(item, advice, { scenario: 'runtime' });
    state.lastAnalysis = { title: advice.title, line: advice.line || (item.focus && item.focus.line) || 0, level: advice.level, ts: Date.now() };
    broadcastStatus();
  } catch (e) {
    Object.assign(item, {
      status: 'error',
      title: '诊断失败',
      error: e.code === 'NO_KEY' ? '还没有填写 API Key，点右上角"设置"填一下就能用了。' : e.message
    });
    updateItem(item);
  }
}

async function analyzeWarnings(evt, logText, parsed) {
  const srcFile = (evt.args || []).find(isSourceFile) || '';
  const id = newId();
  const item = {
    id, type: 'analysis', scenario: 'warning', status: 'loading', ts: Date.now(),
    title: '编译通过，但有小问题', data: null, error: null,
    focus: parsed.warnings[0] ? buildFocus(srcFile, parsed.warnings[0].line, parsed.warnings[0].col, 2) : null
  };
  pushAnalysis(item);
  const userContent = [
    '场景：编译通过，但有警告，学生看不懂。',
    '编译器输出（截断）：\n' + (logText || '').slice(0, 900),
    '警告：\n' + parsed.warnings.slice(0, 3).map((d, i) => `${i + 1}) 第${d.line || '?'}行 ${d.message}`).join('\n'),
    srcFile ? '源代码 ' + path.basename(srcFile) + '：\n' + numberedSource(srcFile, 120) : ''
  ].filter(Boolean).join('\n');
  try {
    const raw = await callAI([
      { role: 'system', content: SYS_COMPILE },
      { role: 'user', content: userContent }
    ]);
    const advice = normalizeAdvice(extractJSON(raw), raw);
    if (item.focus && !advice.badLine) advice.badLine = item.focus.text;
    Object.assign(item, { status: 'done', title: advice.title, data: advice });
    updateItem(item);
    glassFromAdvice(item, advice, { scenario: 'warning' });
    state.lastAnalysis = { title: advice.title, line: advice.line || (item.focus && item.focus.line) || 0, level: 'warning', ts: Date.now() };
    broadcastStatus();
  } catch (e) {
    Object.assign(item, { status: 'error', title: '诊断失败', error: e.code === 'NO_KEY' ? '尚未配置 API Key' : e.message });
    updateItem(item);
  }
}

/* ------------------------------------------------------------ 队列事件处理 */

const processed = new Set();
let scanTimer = null;

function loadProcessed() {
  try {
    const arr = JSON.parse(fs.readFileSync(PROCESSED_FILE, 'utf8'));
    if (Array.isArray(arr)) arr.forEach(x => processed.add(x));
  } catch (e) {}
}

function saveProcessed() {
  try {
    const arr = Array.from(processed).slice(-500);
    fs.writeFileSync(PROCESSED_FILE, JSON.stringify(arr), 'utf8');
  } catch (e) {}
}

function scheduleScan() {
  if (scanTimer) return;
  scanTimer = setTimeout(() => {
    scanTimer = null;
    scanQueue();
  }, 250);
}

function scanQueue() {
  let files = [];
  try { files = fs.readdirSync(QUEUE).filter(f => f.endsWith('.kv')); } catch (e) { return; }
  files.sort((a, b) => {
    const ta = parseInt(a.split('_')[1], 10) || 0;
    const tb = parseInt(b.split('_')[1], 10) || 0;
    return ta - tb;
  });
  for (const f of files) {
    if (processed.has(f)) continue;
    processed.add(f);
    let evt;
    try { evt = kvParse(fs.readFileSync(path.join(QUEUE, f), 'utf8')); } catch (e) { continue; }
    try { handleEvent(evt, f); } catch (e) { console.error('[devai] handleEvent error', e); }
  }
  saveProcessed();
}

function handleEvent(evt, fileName) {
  const srcFile = (evt.args || []).find(isSourceFile) || '';

  if (evt.kind === 'compile_start') {
    if (!state.phase || state.phase === 'idle') {
      state.phase = 'compiling';
      state.source = srcFile;
      state.exe = evt.out || '';
      state.compiler = evt.compiler || '';
      state.lastOk = null;
      broadcastStatus();
      writeGlass({ on: true, kind: 'busy', text: '正在编译 ' + (srcFile ? path.basename(srcFile) : '…') });
    }
    return;
  }

  if (evt.kind === 'compile_end') {
    state.phase = 'idle';
    const logText = evt.log ? smartRead(evt.log) : '';
    const parsed = parseDiagnostics(logText);
    const ok = (parseInt(evt.exit, 10) || 0) === 0;
    state.lastOk = ok;
    state.lastErrors = parsed.errors.length;
    state.source = srcFile || state.source;
    state.lastMs = parseInt(evt.ms, 10) || 0;
    broadcastStatus();

    if (!ok) analyzeCompile(evt, logText, parsed);
    else {
      writeGlass({
        on: true, kind: 'result', ok: true, level: 'ok', line: 0, name: path.basename(state.source || ''),
        title: '编译通过', cause: '没有任何错误，直接点运行试试。',
        where: '', how: [], code: '', tips: parsed.warnings.length ? ['有 ' + parsed.warnings.length + ' 条警告，暂不影响运行'] : [], tip: ''
      });
      if (parsed.warnings.length && loadConfig().explainWarnings) analyzeWarnings(evt, logText, parsed);
    }
    return;
  }

  if (evt.kind === 'run_end') {
    state.phase = 'idle';
    const code = parseInt(evt.exit, 10) || 0;
    state.lastExit = code;
    broadcastStatus();
    if (code === 0) {
      writeGlass({ on: true, kind: 'result', ok: true, level: 'ok', line: 0, title: '运行结束', cause: '程序正常跑完了（退出码 0）。', where: '', how: [], code: '', tips: [], tip: '' });
    } else if (loadConfig().explainRuntime) {
      writeGlass({ on: true, kind: 'busy', text: '运行崩溃（退出码 ' + code + '），小D正在复盘…' });
      analyzeRuntime(evt);
    }
    return;
  }
}

/* --------------------------------------------------------------- HTTP 服务 */

function json(res, code, obj) {
  const body = JSON.stringify(obj);
  res.writeHead(code, { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store' });
  res.end(body);
}

function readBody(req) {
  return new Promise((resolve) => {
    let data = '';
    req.on('data', d => { data += d; if (data.length > 2 * 1024 * 1024) req.destroy(); });
    req.on('end', () => {
      try { resolve(JSON.parse(data || '{}')); } catch (e) { resolve({}); }
    });
  });
}

let lastPanelOpen = 0;

/* dock.exe 在面板存活期间每 200ms 刷新这个心跳文件 */
function panelAlive() {
  try {
    const st = fs.statSync(path.join(STATE_DIR, 'panel.alive'));
    return Date.now() - st.mtimeMs < 4000;
  } catch (e) {
    return false;
  }
}

/* 让已有的面板窗口重新抬到最前（dock.exe 轮询这个数字） */
function raisePanel() {
  try {
    fs.mkdirSync(STATE_DIR, { recursive: true });
    fs.writeFileSync(path.join(STATE_DIR, 'raise.txt'), String(Date.now()), 'utf8');
  } catch (e) {}
}

function openPanel(port) {
  /* 面板已经开着：不要再开第二个，交给 dock.exe 抬到最前就行 */
  if (panelAlive()) {
    raisePanel();
    return true;
  }
  /* 刚开过的几秒里窗口还没写出心跳，先别重复开 */
  if (Date.now() - lastPanelOpen < 12000) {
    raisePanel();
    return true;
  }
  lastPanelOpen = Date.now();
  const urls = [
    'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe',
    'C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe',
    'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe',
    'C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe'
  ];
  const exe = urls.find(p => fs.existsSync(p));
  const target = 'http://127.0.0.1:' + port + '/';
  try {
    if (exe) {
      const child = spawn(exe, [
        '--app=' + target,
        '--user-data-dir=' + path.join(HOME, 'panelprofile'),
        '--window-size=460,960',
        '--window-position=1420,20',
        '--no-first-run',
        '--disable-features=CalculateNativeWinOcclusion,Translate,msEdgeIdentity'
      ], { detached: true, stdio: 'ignore' });
      child.unref();
    } else {
      spawn('cmd', ['/c', 'start', '', target], { detached: true, stdio: 'ignore' }).unref();
    }
    /* 面板窗口出现后，交给 devai_dock.exe 摆放位置/置顶 */
    const dockExe = path.join(HOME, 'bin', 'devai_dock.exe');
    if (fs.existsSync(dockExe)) {
      const dock = spawn(dockExe, [
        '--host=DevCpp.exe',
        '--panel=DevAI 小助手',
        '--width=440'
      ], { detached: true, stdio: 'ignore', windowsHide: true });
      dock.unref();
      raisePanel();
    }
    return true;
  } catch (e) {
    return false;
  }
}

/* 停靠模式：dock（嵌进 Dev-C++）/ float（独立浮窗），dock.exe 定时读这个文件 */
function setDockMode(mode) {
  const f = path.join(STATE_DIR, 'dockmode.txt');
  try { fs.mkdirSync(STATE_DIR, { recursive: true }); } catch (e) {}
  const v = mode === 'float' ? 'float' : 'dock';
  fs.writeFileSync(f, v, 'utf8');
  return v;
}

function readDockMode() {
  try {
    const v = fs.readFileSync(path.join(STATE_DIR, 'dockmode.txt'), 'utf8').trim();
    return v === 'float' ? 'float' : 'dock';
  } catch (e) {
    return 'dock';
  }
}

function serveStatic(req, res, pathname) {
  let file = path.join(UI_DIR, pathname === '/' ? 'index.html' : pathname.replace(/^\//, ''));
  if (!path.resolve(file).startsWith(path.resolve(UI_DIR))) { res.writeHead(403); return res.end('forbidden'); }
  fs.readFile(file, (err, buf) => {
    if (err) { res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' }); return res.end('not found'); }
    const ext = path.extname(file).toLowerCase();
    const types = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8', '.svg': 'image/svg+xml', '.png': 'image/png' };
    res.writeHead(200, { 'Content-Type': types[ext] || 'application/octet-stream', 'Cache-Control': 'no-store' });
    res.end(buf);
  });
}

const server = http.createServer(async (req, res) => {
  const u = new URL(req.url, 'http://127.0.0.1');
  const p = u.pathname;

  if (req.method === 'GET' && p === '/api/stream') {
    res.writeHead(200, {
      'Content-Type': 'text/event-stream; charset=utf-8',
      'Cache-Control': 'no-cache, no-transform',
      Connection: 'keep-alive',
      'X-Accel-Buffering': 'no'
    });
    res.write('retry: 3000\n\n');
    clients.add(res);
    res.write('data: ' + JSON.stringify({ t: 'snapshot', feed, state, config: maskConfig(loadConfig()), dockMode: readDockMode() }) + '\n\n');
    const ping = setInterval(() => { try { res.write(': ping\n\n'); } catch (e) {} }, 15000);
    req.on('close', () => { clearInterval(ping); clients.delete(res); });
    return;
  }

  if (req.method === 'GET' && p === '/api/health') {
    return json(res, 200, { app: 'devai', pid: process.pid, port: actualPort });
  }

  if (req.method === 'GET' && p === '/api/config') return json(res, 200, maskConfig(loadConfig()));

  if (req.method === 'POST' && p === '/api/config') {
    const body = await readBody(req);
    const cfg = saveConfig(body);
    broadcast({ t: 'config', config: maskConfig(cfg) });
    return json(res, 200, { ok: true, config: maskConfig(cfg) });
  }

  if (req.method === 'POST' && p === '/api/test') {
    try {
      const raw = await callAI([
        { role: 'system', content: '只输出 JSON：{"ok":true,"msg":"连接成功"}' },
        { role: 'user', content: '请返回 {"ok":true,"msg":"连接成功"}' }
      ], { timeout: 30000 });
      const obj = extractJSON(raw);
      return json(res, 200, { ok: true, model: loadConfig().model, sample: obj || raw.slice(0, 120) });
    } catch (e) {
      return json(res, 200, { ok: false, error: e.message });
    }
  }

  if (req.method === 'POST' && p === '/api/reanalyze') {
    if (!lastScenario) return json(res, 200, { ok: false, error: '还没有可分析的记录，先编译一次吧' });
    const sc = lastScenario;
    lastAnalysisKey = '';
    if (sc.kind === 'compile') analyzeCompile(sc.evt, sc.logText, sc.parsed);
    else if (sc.kind === 'runtime') analyzeRuntime(sc.evt);
    return json(res, 200, { ok: true });
  }

  if (req.method === 'POST' && p === '/api/review') {
    const body = await readBody(req);
    const file = body.file || state.source || '';
    if (!file || !fs.existsSync(file)) return json(res, 200, { ok: false, error: '找不到源文件，先编译一次或打开一个文件' });
    const id = newId();
    const item = { id, type: 'analysis', scenario: 'review', status: 'loading', ts: Date.now(), title: '代码体检中…', data: null, error: null };
    pushAnalysis(item);
    json(res, 200, { ok: true, id });
    try {
      const raw = await callAI([
        { role: 'system', content: SYS_REVIEW },
        { role: 'user', content: '场景：代码体检，挑出最可能的坑。\n文件 ' + path.basename(file) + '：\n' + numberedSource(file, 140) }
      ]);
      const advice = normalizeAdvice(extractJSON(raw), raw);
      if (advice.line) item.focus = buildFocus(file, advice.line, null, 2);
      item.file = file;
      if (item.focus && !advice.badLine) advice.badLine = item.focus.text;
      Object.assign(item, { status: 'done', title: advice.title, data: advice });
      updateItem(item);
      glassFromAdvice(item, advice, { scenario: 'review', file });
      state.lastAnalysis = { title: advice.title, line: advice.line || 0, level: advice.level, ts: Date.now() };
      broadcastStatus();
    } catch (e) {
      Object.assign(item, { status: 'error', title: '体检失败', error: e.code === 'NO_KEY' ? '尚未配置 API Key' : e.message });
      updateItem(item);
    }
    return;
  }

  if (req.method === 'POST' && p === '/api/chat') {
    const body = await readBody(req);
    const q = (body.question || '').trim();
    if (!q) return json(res, 200, { ok: false, error: '问题是空的' });
    const file = body.file || state.source || '';
    const ctx = file && fs.existsSync(file) ? '【当前代码 ' + path.basename(file) + '（节选）】\n' + numberedSource(file, 120) : '';
    chatHistory.push({ role: 'user', content: q });
    if (chatHistory.length > 10) chatHistory = chatHistory.slice(-10);
    try {
      const raw = await callAI([
        { role: 'system', content: SYS_CHAT },
        { role: 'system', content: ctx || '（没有可用代码上下文）' },
        ...chatHistory
      ], { json: false, timeout: 60000 });
      chatHistory.push({ role: 'assistant', content: raw });
      return json(res, 200, { ok: true, answer: raw });
    } catch (e) {
      return json(res, 200, { ok: false, error: e.code === 'NO_KEY' ? '请先在"设置"里填写 API Key' : e.message });
    }
  }

  if (req.method === 'GET' && p === '/api/source') {
    const file = u.searchParams.get('file') || state.source;
    if (!file || !fs.existsSync(file)) return json(res, 200, { ok: false });
    return json(res, 200, { ok: true, file, code: numberedSource(file, 400) });
  }

  if (req.method === 'GET' && p === '/api/dockmode') return json(res, 200, { mode: readDockMode() });

  if (req.method === 'POST' && p === '/api/dockmode') {
    const body = await readBody(req);
    const mode = setDockMode(body.mode);
    return json(res, 200, { ok: true, mode });
  }

  if (req.method === 'POST' && p === '/api/panelmetrics') {
    const body = await readBody(req);
    const dpr = Number(body.dpr);
    if (dpr > 0.5 && dpr < 5) {
      try { fs.writeFileSync(path.join(STATE_DIR, 'dpr.txt'), String(dpr), 'utf8'); } catch (e) {}
    }
    return json(res, 200, { ok: true });
  }

  if (req.method === 'POST' && p === '/api/show-panel') {
    openPanel(actualPort);
    return json(res, 200, { ok: true });
  }

  if (req.method === 'GET') return serveStatic(req, res, p);

  res.writeHead(404); res.end('not found');
});

function maskConfig(cfg) {
  const c = Object.assign({}, cfg);
  if (c.apiKey) c.apiKeyMasked = c.apiKey.slice(0, 4) + '••••••••' + c.apiKey.slice(-4);
  c.apiKeySet = !!cfg.apiKey;
  delete c.apiKey;
  return c;
}

/* ---------------------------------------------------------------- 启动 */

let actualPort = 0;

function watchQueue() {
  try {
    fs.watch(QUEUE, () => scheduleScan());
  } catch (e) {}
  setInterval(scanQueue, 1500);
}

function tryListen(port, attempt) {
  server.once('error', (err) => {
    if (err.code === 'EADDRINUSE') {
      /* 端口被占：先看看是不是已经有一个 DevAI 在跑，是的话直接退出，避免双开抢事件 */
      fetch('http://127.0.0.1:' + port + '/api/health')
        .then(r => r.json())
        .then(j => {
          if (j && j.app === 'devai') {
            try { fs.writeFileSync(PORT_FILE, String(port), 'utf8'); } catch (e) {}
            console.log('[devai] already running on ' + port + ', exit.');
            process.exit(0);
          }
          throw new Error('not devai');
        })
        .catch(() => {
          if (attempt < 25) tryListen(port + 1, attempt + 1);
          else console.error('[devai] no free port around ' + port);
        });
    } else {
      console.error('[devai] listen error', err);
    }
  });
  server.listen(port, '127.0.0.1', () => {
    actualPort = port;
    try { fs.writeFileSync(PORT_FILE, String(port), 'utf8'); } catch (e) {}
    try { fs.writeFileSync(PID_FILE, String(process.pid), 'utf8'); } catch (e) {}
    console.log('[devai] ready on http://127.0.0.1:' + port);
    const cfg = loadConfig();
    if (cfg.autoOpenPanel && !process.env.DEVAI_NO_PANEL) {
      setTimeout(() => openPanel(port), 400);
    }
  });
}

function main() {
  ensureDirs();
  if (!fs.existsSync(CONFIG_FILE)) saveConfig({});
  if (!fs.existsSync(path.join(STATE_DIR, 'dockmode.txt'))) setDockMode('dock');
  loadProcessed();
  watchQueue();
  scanQueue();
  ensureGlass();
  tryListen(loadConfig().port || 47901, 0);

  process.on('SIGINT', shutdown);
  process.on('SIGTERM', shutdown);
  process.on('exit', () => { try { fs.unlinkSync(PID_FILE); } catch (e) {} });
}

function shutdown() {
  try { fs.unlinkSync(PID_FILE); } catch (e) {}
  process.exit(0);
}

main();
