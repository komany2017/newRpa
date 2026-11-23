/* MV3 service worker: owns the single Native Messaging port, exposes a
   promise-based RPC surface, and persists bridge diagnostics for the popup. */
'use strict';

const HOST_NAME = 'com.workbuddy.rpa_picker';
const RPC_TIMEOUT_MS = 8000;
const DIAG_KEY = 'rpaLastDiagnostic';
const FAIL_KEY = 'rpaLastFailure';
const PICK_KEY = 'rpaLastPickDescriptor';

let port = null;
let nextId = 1;
let connectionError = null;
const pending = new Map();

function broadcast(msg) {
  try {
    const p = chrome.runtime.sendMessage(msg);
    if (p && typeof p.catch === 'function') p.catch(() => {});
  } catch (e) { /* no receiver is fine */ }
}

// Turn raw Chrome / C++ errors into an actionable, human-readable hint.
function classifyError(e) {
  const raw = String(e && e.message || e);
  if (/Specified native messaging host not found|native messaging host not found|Access to the specified native messaging host|forbidden/i.test(raw)) {
    return {
      code: 'HOST_NOT_REGISTERED', error: raw,
      hint: '本机主机未注册或扩展 ID 不匹配：在普通终端运行 scripts\\register_host.bat <扩展ID>，再到扩展页点「重新加载」',
    };
  }
  if (/Native host has exited|host exited|disconnected|error when opening/i.test(raw)) {
    return {
      code: 'HOST_EXITED', error: raw,
      hint: '本机进程启动后立即退出：检查 build\\rpa_native_host.exe 是否存在（可能被安全软件清理，重跑 build.bat），再看 host 日志',
    };
  }
  if (/remote-debugging-port|cannot reach 127\.0\.0\.1/i.test(raw)) {
    return {
      code: 'CDP_UNAVAILABLE', error: raw,
      hint: '调试浏览器未启动：双击 launch_browser.bat，或用 --remote-debugging-port=9222 启动浏览器',
    };
  }
  if (/timeout/i.test(raw)) {
    return { code: 'TIMEOUT', error: raw, hint: '本机引擎 8 秒无响应：检查 host 日志与 CDP 端口' };
  }
  return { code: 'UNKNOWN', error: raw, hint: raw };
}

function ensurePort() {
  if (port) return port;
  try {
    connectionError = null;
    const candidate = chrome.runtime.connectNative(HOST_NAME);
    if (!candidate) throw new Error('connectNative returned no port');

    candidate.onMessage.addListener((msg) => {
      if (msg && msg.id != null && pending.has(msg.id)) {
        const item = pending.get(msg.id);
        pending.delete(msg.id);
        clearTimeout(item.timer);
        if (msg.ok) item.resolve(msg);
        else item.reject(new Error(msg.error || 'native host returned an error'));
        return;
      }
      broadcast({ type: 'hostMessage', message: msg });
    });

    candidate.onDisconnect.addListener(() => {
      connectionError = (chrome.runtime.lastError && chrome.runtime.lastError.message) ||
        'native host disconnected';
      port = null;
      for (const item of pending.values()) {
        clearTimeout(item.timer);
        item.reject(new Error(connectionError));
      }
      pending.clear();
      broadcast({ type: 'hostStatus', connected: false, error: connectionError });
    });
    port = candidate;
  } catch (e) {
    connectionError = String(e && e.message || e);
    port = null;
  }
  return port;
}

function rpc(type, payload) {
  return new Promise((resolve, reject) => {
    const p = ensurePort();
    if (!p) { reject(new Error(connectionError || 'native port unavailable')); return; }
    const id = nextId++;
    const timer = setTimeout(() => {
      pending.delete(id);
      reject(new Error('native host timeout (8s)'));
    }, RPC_TIMEOUT_MS);
    pending.set(id, { resolve, reject, timer });
    try {
      p.postMessage({ id, type, payload: payload || {} });
    } catch (e) {
      pending.delete(id);
      clearTimeout(timer);
      reject(e);
    }
  });
}

async function storageSet(obj) {
  try { await chrome.storage.local.set(obj); } catch (e) { /* storage optional */ }
}
async function storageGet(keys) {
  try { return await chrome.storage.local.get(keys); } catch (e) { return {}; }
}
async function saveDescriptor(descriptor) {
  try { await chrome.storage.session.set({ lastDescriptor: descriptor }); } catch (e) { /* optional */ }
}

chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  const reply = typeof sendResponse === 'function' ? sendResponse : () => {};

  // A throwing listener leaves the message port unanswered. Never throw here.
  if (!msg || typeof msg !== 'object') {
    reply({ ok: false, error: 'invalid message' });
    return true;
  }

  const handle = async () => {
    switch (msg.type) {
      case 'wakeup':          // cheap SW liveness probe, never touches the host
        return { ready: true, connected: !!port, error: connectionError };
      case 'hostStatus':
        return { connected: !!port, error: connectionError };
      case 'bridgeDiagnostics': {
        const d = await storageGet([DIAG_KEY, FAIL_KEY, PICK_KEY, 'settings']);
        return {
          last: d[DIAG_KEY] || null,
          failure: d[FAIL_KEY] || null,
          hasDescriptor: !!d[PICK_KEY],
          settings: d.settings || {},
          extensionId: chrome.runtime.id,
          host: HOST_NAME,
          connectionError: connectionError,
        };
      }
      case 'resendLastDescriptor': {
        const d = await storageGet([PICK_KEY]);
        if (!d[PICK_KEY]) throw new Error('没有可重发的 descriptor，请先拾取元素');
        return rpc('pickResult', { descriptor: d[PICK_KEY] });
      }
      case 'getVersion':
        return rpc('getVersion');
      case 'hostPing':
        return rpc('ping', msg.payload || {});
      case 'configure':
        return rpc('configure', msg.payload || {});
      case 'pickResult': {
        if (!msg.payload || !msg.payload.descriptor) throw new Error('missing descriptor');
        // Persist BEFORE contacting the host: a failed bridge must not lose the pick.
        await saveDescriptor(msg.payload.descriptor);
        await storageSet({ [PICK_KEY]: msg.payload.descriptor });
        return rpc('pickResult', msg.payload);
      }
      case 'replay':
        return rpc('replay', msg.payload || {});
      case 'execute':
        return rpc('execute', msg.payload || {});
      case 'debugScript':
        return rpc('debugScript', msg.payload || {});
      case 'evaluateRaw':
        return rpc('evaluateRaw', msg.payload || {});
      default:
        throw new Error('unknown background message: ' + msg.type);
    }
  };

  handle()
    .then(async (data) => {
      if (msg.type !== 'wakeup' && msg.type !== 'hostStatus' && msg.type !== 'bridgeDiagnostics') {
        await storageSet({ [DIAG_KEY]: {
          ok: true, at: Date.now(), type: msg.type, error: null, hint: '本机引擎响应正常',
        } });
      }
      reply({ ok: true, data });
    })
    .catch(async (e) => {
      const info = classifyError(e);
      const rec = Object.assign({ ok: false, at: Date.now(), type: msg.type,
        host: HOST_NAME, extensionId: chrome.runtime.id }, info);
      await storageSet({ [DIAG_KEY]: rec, [FAIL_KEY]: rec });
      reply({ ok: false, error: info.error, hint: info.hint, code: info.code });
    });
  return true;  // keep the message channel open for the async response
});

chrome.runtime.onInstalled.addListener(() => {
  try { chrome.storage.session.setAccessLevel({ accessLevel: 'TRUSTED_AND_UNTRUSTED_CONTEXTS' }); }
  catch (e) { /* older Chrome */ }
});
