/* Injected into every frame (manifest + on-demand fallback injection).
   Mouse hover highlights the element; click picks it, builds a descriptor via
   selector.js and pushes it to the native host. composedPath() keeps elements
   inside open Shadow DOM pickable. */
(function () {
  'use strict';

  // Both the manifest and popup-side scripting.executeScript may load this
  // file in the same isolated world. Register listeners exactly once.
  if (window.__RPA_PICKER_INSTALLED__) return;
  const S = window.RpaSelector;
  if (!S) {
    console.warn('[rpa-picker] selector.js is not loaded yet');
    return;
  }
  window.__RPA_PICKER_INSTALLED__ = true;

  const ROOT_ID = '__rpa_picker_root__';
  let rootEl = null;
  let boxEl = null;
  let labelEl = null;
  let toastEl = null;
  let toastTimer = null;
  let active = false;
  let current = null;

  function isOwnUI(node) {
    return !!(node && node.nodeType === 1 && node.closest && node.closest('#' + ROOT_ID));
  }

  function ensureUI() {
    if (rootEl && rootEl.isConnected) return;
    rootEl = document.createElement('div');
    rootEl.id = ROOT_ID;

    boxEl = document.createElement('div');
    boxEl.className = 'rpa-box';
    labelEl = document.createElement('div');
    labelEl.className = 'rpa-label';
    toastEl = document.createElement('div');
    toastEl.className = 'rpa-toast';

    rootEl.appendChild(boxEl);
    rootEl.appendChild(labelEl);
    rootEl.appendChild(toastEl);
    (document.documentElement || document.body).appendChild(rootEl);
  }

  function targetFrom(e) {
    const path = typeof e.composedPath === 'function' ? e.composedPath() : [e.target];
    for (const node of path) {
      if (node && node.nodeType === 1 && !isOwnUI(node)) return node;
    }
    return null;
  }

  function showBox(el) {
    const r = el.getBoundingClientRect();
    const text = String(el.innerText || el.textContent || '').replace(/\s+/g, ' ').trim().slice(0, 40);
    boxEl.style.display = 'block';
    boxEl.style.left = r.left + 'px';
    boxEl.style.top = r.top + 'px';
    boxEl.style.width = r.width + 'px';
    boxEl.style.height = r.height + 'px';
    labelEl.style.display = 'block';
    labelEl.style.left = Math.max(0, r.left) + 'px';
    labelEl.style.top = Math.max(0, r.top - 24) + 'px';
    labelEl.textContent = '<' + String(el.tagName).toLowerCase() +
      (el.id ? '#' + el.id : '') + '> ' + text;
  }

  function hideBox() {
    if (boxEl) boxEl.style.display = 'none';
    if (labelEl) labelEl.style.display = 'none';
  }

  function toast(text, good, duration) {
    ensureUI();
    toastEl.textContent = text;
    toastEl.className = 'rpa-toast ' + (good ? 'rpa-ok' : 'rpa-err');
    toastEl.style.display = 'block';
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { toastEl.style.display = 'none'; }, duration || 4000);
  }

  // A MV3 service worker can still be starting up when the first message is
  // sent. Retry once, and translate the raw Chrome error for the user.
  function sendToBackground(msg) {
    return new Promise(function (resolve, reject) {
      function attempt(retriesLeft) {
        let result;
        try {
          result = chrome.runtime.sendMessage(msg);
        } catch (e) {
          return fail(e, retriesLeft);
        }
        Promise.resolve(result).then(resolve, function (e) { fail(e, retriesLeft); });
      }
      function fail(e, retriesLeft) {
        const raw = String(e && e.message || e);
        if (retriesLeft > 0 && /Receiving end does not exist|message port closed/i.test(raw)) {
          setTimeout(function () { attempt(retriesLeft - 1); }, 250);
        } else {
          reject(new Error(/Receiving end does not exist/i.test(raw)
            ? '扩展后台未就绪：请在扩展页点「重新加载」并刷新页面'
            : raw));
        }
      }
      attempt(1);
    });
  }

  function onMove(e) {
    if (!active) return;
    const el = targetFrom(e);
    if (!el) { hideBox(); return; }
    current = el;
    showBox(el);
  }

  function onClick(e) {
    if (!active) return;
    e.preventDefault();
    e.stopPropagation();
    if (e.stopImmediatePropagation) e.stopImmediatePropagation();

    const el = targetFrom(e) || current;
    if (!el) return;
    let descriptor;
    try {
      descriptor = S.buildDescriptor(el);
    } catch (err) {
      toast('拾取失败: ' + (err && err.message ? err.message : err), false);
      return;
    }
    const label = '<' + descriptor.meta.tag + '> ' + (descriptor.meta.text || descriptor.meta.id || '');
    // Persist locally so the popup can resend even when the bridge is down.
    try {
      if (chrome.storage && chrome.storage.local) {
        chrome.storage.local.set({ rpaLastPickDescriptor: descriptor });
      }
    } catch (e) { /* storage optional */ }
    sendToBackground({ type: 'pickResult', payload: { descriptor: descriptor } })
      .then(function (res) {
        if (res && res.ok) {
          toast('已拾取 ' + label + ' · ' + descriptor.candidates.length + ' 个候选已同步到本机引擎', true);
        } else {
          const hint = (res && (res.hint || res.error)) || '未知错误';
          toast('桥接失败: ' + hint + '｜可在扩展 popup 的「桥接诊断」查看并重试', false, 9000);
        }
      })
      .catch(function (err) {
        toast('桥接异常: ' + (err && err.message ? err.message : err), false);
      });
    stop();
  }

  function onKey(e) {
    if (e.key === 'Escape' && active) stop();
  }

  function start() {
    ensureUI();
    if (active) return;
    active = true;
    document.addEventListener('mousemove', onMove, true);
    document.addEventListener('click', onClick, true);
    document.addEventListener('keydown', onKey, true);
    toast('拾取模式：移动鼠标选元素，单击确认，Esc 退出', true);
  }

  function stop() {
    if (!active) return;
    active = false;
    current = null;
    document.removeEventListener('mousemove', onMove, true);
    document.removeEventListener('click', onClick, true);
    document.removeEventListener('keydown', onKey, true);
    hideBox();
  }

  // Always answer, including unknown/broadcast messages: an unanswered
  // listener closes the port and surfaces as "Receiving end does not exist".
  chrome.runtime.onMessage.addListener(function (msg, sender, sendResponse) {
    const reply = typeof sendResponse === 'function' ? sendResponse : function () {};
    if (!msg || typeof msg !== 'object') {
      reply({ ok: false, error: 'invalid message' });
      return;
    }
    if (msg.type === 'pickerStart') {
      start();
      reply({ ok: true, active: true, frame: window.location.href });
    } else if (msg.type === 'pickerStop') {
      stop();
      reply({ ok: true, active: false, frame: window.location.href });
    } else if (msg.type === 'pickerStatus') {
      reply({ ok: true, active: active, frame: window.location.href });
    } else {
      reply({ ok: true, ignored: true });
    }
  });
})();
