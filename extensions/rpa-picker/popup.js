'use strict';

const $ = (id) => document.getElementById(id);
let descriptor = null;
const BLOCKED_URL = /^(chrome|edge|about|chrome-extension|devtools|view-source|data):/i;

function sleep(ms) { return new Promise((r) => setTimeout(r, ms)); }

function friendlyError(e) {
  const raw = String(e && e.message || e);
  if (/Receiving end does not exist/i.test(raw)) {
    return '扩展后台未就绪：请在扩展页点「重新加载」，然后刷新目标页面';
  }
  if (/message port closed/i.test(raw)) {
    return '消息通道提前关闭：请重新加载扩展并刷新页面';
  }
  if (/Specified native messaging host not found|Access to the specified native messaging host/i.test(raw)) {
    return '本机主机未注册或扩展 ID 不匹配：请运行 scripts\\register_host.bat <扩展ID> 并重新加载扩展';
  }
  if (/Cannot access contents|Missing host permission|cannot be scripted|Cannot access a chrome/i.test(raw)) {
    return '当前页面不允许注入脚本（chrome://、扩展商店页等）';
  }
  return raw;
}

// Retry once on a cold MV3 service worker; prefer the backend's actionable hint.
async function call(msg) {
  let r;
  try {
    r = await chrome.runtime.sendMessage(msg);
  } catch (e) {
    const raw = String(e && e.message || e);
    if (!/Receiving end does not exist/i.test(raw)) throw new Error(friendlyError(e));
    await sleep(250);
    try {
      r = await chrome.runtime.sendMessage(msg);
    } catch (e2) {
      throw new Error(friendlyError(e2));
    }
  }
  if (!r) throw new Error('后台服务未响应（service worker 可能正在休眠，请重试）');
  if (!r.ok) throw new Error(r.hint || r.error || '后台返回错误');
  return r.data;
}

async function copy(text) {
  try {
    await navigator.clipboard.writeText(text);
    setStatus('已复制到剪贴板', true);
  } catch (e) {
    setStatus('复制失败: ' + friendlyError(e), false);
  }
}

function setStatus(text, good) {
  const el = $('hostStatus');
  el.textContent = text;
  el.className = 'status ' + (good ? 'ok' : 'err');
}

function showResult(value) {
  $('result').textContent = typeof value === 'string' ? value : JSON.stringify(value, null, 2);
}

function renderDescriptor() {
  const info = $('targetInfo');
  const list = $('candidates');
  list.textContent = '';
  if (!descriptor) {
    info.textContent = '尚未拾取元素';
    return;
  }
  const m = descriptor.meta || {};
  const cs = descriptor.candidates || [];
  const fp = descriptor.framePath || [];
  const sh = descriptor.shadowHosts || [];
  info.textContent = '<' + (m.tag || '?') + (m.id ? '#' + m.id : '') + '> ' +
    (m.text || '') + ' · ' + cs.length + ' 候选 · framePath=[' + fp.join(',') + ']' +
    (sh.length ? ' · shadow=' + sh.length : '');

  cs.slice(0, 8).forEach((c) => {
    const li = document.createElement('li');
    li.title = '点击复制该选择器';
    const score = document.createElement('span');
    score.className = 'score';
    score.textContent = String(c.score);
    const code = document.createElement('code');
    code.textContent = c.type + ': ' + c.value;  // textContent: never inject markup
    li.appendChild(score);
    li.appendChild(code);
    li.addEventListener('click', () => copy(c.value));
    list.appendChild(li);
  });
}

function renderDiagnostic(rec) {
  const el = $('diagInfo');
  if (!rec) { el.textContent = '尚未记录桥接事件'; return; }
  const time = rec.at ? new Date(rec.at).toLocaleTimeString() : '';
  el.textContent = (rec.ok ? '[成功] ' : '[失败] ') + (rec.type || '') + ' ' + time + '\n' +
    (rec.hint || rec.error || '');
}

async function activeTab() {
  const tabs = await chrome.tabs.query({ active: true, currentWindow: true });
  if (!tabs || !tabs.length) throw new Error('没有活动标签页');
  return tabs[0];
}

// The #1 cause of "Receiving end does not exist": the page was open before the
// extension was loaded, so the manifest content script was never injected.
// Probe first, then inject on demand (idempotent thanks to the content guard).
async function ensureContentScript(tab) {
  if (!tab || typeof tab.id !== 'number') throw new Error('没有活动标签页');
  const url = tab.url || '';
  if (BLOCKED_URL.test(url)) {
    throw new Error('当前页面不支持注入脚本：请切到普通 http(s) 页面');
  }

  try {
    const probe = await chrome.tabs.sendMessage(tab.id, { type: 'pickerStatus' });
    if (probe && probe.ok) return;
  } catch (e) { /* content script absent -> inject below */ }

  const target = { tabId: tab.id, allFrames: true };
  try {
    await chrome.scripting.insertCSS({ target: target, files: ['picker.css'] });
    await chrome.scripting.executeScript({ target: target, files: ['selector.js', 'content.js'] });
  } catch (e) {
    const hint = /^file:/i.test(url) ? '（file:// 需在扩展详情开启「允许访问文件网址」）' : '';
    throw new Error('自动注入失败：' + friendlyError(e) + hint + '，或刷新页面后重试');
  }

  try {
    const probe2 = await chrome.tabs.sendMessage(tab.id, { type: 'pickerStatus' });
    if (probe2 && probe2.ok) return;
  } catch (e) { /* fall through to the clear error */ }
  throw new Error('content script 仍未就绪，请刷新目标页面后重试');
}

function payload() {
  return descriptor ? { descriptor: descriptor } : {};
}

async function init() {
  let stored = {};
  try { stored = await chrome.storage.session.get(['lastDescriptor', 'settings']); } catch (e) { /* ignore */ }
  descriptor = stored.lastDescriptor || null;
  const s = stored.settings || {};
  $('cdpPort').value = s.cdpPort || 9222;
  $('targetMatch').value = s.targetMatch || '';
  renderDescriptor();

  try {
    const diag = await chrome.storage.local.get(['rpaLastFailure', 'rpaLastDiagnostic']);
    renderDiagnostic(diag.rpaLastFailure || diag.rpaLastDiagnostic);
  } catch (e) { /* storage optional */ }

  try {
    await call({ type: 'wakeup' });  // distinguish SW problems from host problems
  } catch (e) { /* getVersion below reports the real cause */ }

  try {
    const v = await call({ type: 'getVersion' });
    const p = v.payload || {};
    setStatus('本机引擎已连接 · v' + p.version + ' · PID ' + p.pid + ' · CDP ' + p.cdpPort, true);
  } catch (e) {
    setStatus('本机引擎未连接：' + friendlyError(e), false);
    try {
      const diag = await chrome.storage.local.get(['rpaLastFailure']);
      renderDiagnostic(diag.rpaLastFailure);
    } catch (x) { /* ignore */ }
  }
}

$('btnStart').addEventListener('click', async () => {
  try {
    const tab = await activeTab();
    await ensureContentScript(tab);
    const r = await chrome.tabs.sendMessage(tab.id, { type: 'pickerStart' });
    if (r && r.ok === false) throw new Error(r.error || '拾取器未响应');
    window.close();
  } catch (e) {
    setStatus('无法启动拾取: ' + friendlyError(e), false);
  }
});

$('btnStop').addEventListener('click', async () => {
  try {
    const tab = await activeTab();
    await chrome.tabs.sendMessage(tab.id, { type: 'pickerStop' });
    setStatus('已停止拾取', true);
  } catch (e) {
    setStatus('停止失败（拾取器未运行）: ' + friendlyError(e), false);
  }
});

$('btnCopy').addEventListener('click', () => {
  if (!descriptor) { setStatus('请先拾取元素', false); return; }
  copy(JSON.stringify(descriptor, null, 2));
});

$('btnResend').addEventListener('click', async () => {
  try {
    const d = await chrome.storage.local.get(['rpaLastPickDescriptor']);
    const dsc = d.rpaLastPickDescriptor;
    if (!dsc) { setStatus('没有可重发的 descriptor，请先拾取元素', false); return; }
    const r = await call({ type: 'resendLastDescriptor' });
    descriptor = dsc;
    renderDescriptor();
    renderDiagnostic({ ok: true, at: Date.now(), type: 'resendLastDescriptor', hint: '重新同步成功' });
    const cnt = r && r.payload ? r.payload.candidateCount : 0;
    setStatus('已重新同步到本机引擎 · ' + cnt + ' 个候选', true);
  } catch (e) {
    setStatus('重新同步失败: ' + friendlyError(e), false);
    try {
      const diag = await chrome.storage.local.get(['rpaLastFailure']);
      renderDiagnostic(diag.rpaLastFailure);
    } catch (x) { /* ignore */ }
  }
});

$('btnCopyDiag').addEventListener('click', async () => {
  try {
    const info = await call({ type: 'bridgeDiagnostics' });
    const local = await chrome.storage.local.get([
      'rpaLastFailure', 'rpaLastDiagnostic', 'rpaLastPickDescriptor', 'settings']);
    const report = {
      collectedAt: new Date().toISOString(),
      extensionId: info.extensionId,
      host: info.host,
      connectionError: info.connectionError,
      settings: local.settings || {},
      hasDescriptor: !!local.rpaLastPickDescriptor,
      lastDiagnostic: local.rpaLastDiagnostic || null,
      lastFailure: local.rpaLastFailure || null,
      hint: 'code=HOST_NOT_REGISTERED 时，在普通终端运行 scripts\\register_host.bat <扩展ID>',
    };
    await copy(JSON.stringify(report, null, 2));
  } catch (e) {
    setStatus('复制诊断失败: ' + friendlyError(e), false);
  }
});

$('btnReplay').addEventListener('click', async () => {
  if (!descriptor) { setStatus('请先拾取元素', false); return; }
  try {
    const r = await call({ type: 'replay', payload: payload() });
    const result = (r.payload && r.payload.result) || {};
    showResult(result);
    setStatus(result.found ? '回放命中 · ' + (result.matched ? result.matched.value : '') : '未命中唯一元素',
      !!result.found);
  } catch (e) { setStatus('回放失败: ' + friendlyError(e), false); }
});

$('btnExecute').addEventListener('click', async () => {
  if (!descriptor) { setStatus('请先拾取元素', false); return; }
  const type = $('actionType').value;
  const raw = $('actionValue').value;
  const action = { type: type };
  if (type === 'input') action.value = raw;
  if (type === 'getAttribute') action.name = raw || 'data-testid';
  try {
    const r = await call({ type: 'execute', payload: { descriptor: descriptor, action: action } });
    const result = (r.payload && r.payload.result) || {};
    showResult(result);
    setStatus(result.found ? '动作完成 · ' + (result.action || type) : '未命中唯一元素', !!result.found);
  } catch (e) { setStatus('执行失败: ' + friendlyError(e), false); }
});

$('btnSave').addEventListener('click', async () => {
  const settings = {
    cdpPort: Number($('cdpPort').value) || 9222,
    targetMatch: $('targetMatch').value.trim(),
  };
  try {
    await chrome.storage.session.set({ settings: settings });
    await call({ type: 'configure', payload: settings });
    setStatus('配置已保存 · CDP 端口 ' + settings.cdpPort, true);
  } catch (e) { setStatus('配置失败: ' + friendlyError(e), false); }
});

$('btnPing').addEventListener('click', async () => {
  try {
    const r = await call({ type: 'hostPing' });
    setStatus('PONG · 引擎累计处理 ' + (r.payload.requestCount || 0) + ' 条消息', true);
  } catch (e) { setStatus('PING 失败: ' + friendlyError(e), false); }
});

init();
