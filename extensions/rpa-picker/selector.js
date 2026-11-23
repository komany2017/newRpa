/* RPA element selector engine.
 *
 * Loaded as a classic script in the MV3 content script (window.RpaSelector)
 * and as CommonJS in Node tests. generateCandidates() and
 * buildDescriptorFromInfo() are pure functions; only buildDescriptor()
 * touches the DOM.
 *
 * Descriptor contract consumed by native/locator.h:
 *   { version, candidates:[{type,value,score}], framePath:[int], shadowHosts:[css],
 *     fallback:{tag,text,index}, meta:{tag,id,text,url,title,path,ts} }
 */
(function (root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  if (root) root.RpaSelector = api;
})(typeof globalThis !== 'undefined' ? globalThis : this, function () {
  'use strict';

  const ATTR_KEYS = [
    'data-testid', 'data-test', 'data-qa', 'data-cy', 'data-automation-id', 'data-id',
    'name', 'aria-label', 'aria-labelledby', 'placeholder', 'title', 'type', 'role',
    'href', 'alt', 'for', 'contenteditable', 'tabindex',
  ];

  const TEST_ATTRS = [
    { key: 'data-testid', score: 94 },
    { key: 'data-test', score: 92 },
    { key: 'data-qa', score: 92 },
    { key: 'data-cy', score: 92 },
    { key: 'data-automation-id', score: 92 },
    { key: 'data-id', score: 86 },
  ];

  const STATE_CLASSES = new Set([
    'active', 'hover', 'focus', 'focused', 'selected', 'disabled', 'checked',
    'open', 'closed', 'hidden', 'show', 'hide', 'current', 'visited', 'loading',
    'error', 'success', 'pending', 'expanded', 'collapsed', 'dragging', 'readonly',
  ]);

  const DYNAMIC_ID_PREFIX = /^(ember|react|angular|vue|extjs|ext-|yui|mui|css-|sc-|jsx-|radix-|chakra-|ant-|el-|mat-|v-|:r[0-9a-z]?:|__)/i;

  function isDynamicId(id) {
    const s = String(id || '');
    if (!s) return true;
    if (/^\d+$/.test(s)) return true;
    if (DYNAMIC_ID_PREFIX.test(s)) return true;
    if (/\d{5,}/.test(s)) return true;
    if (/[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}/i.test(s)) return true;
    if (/^[0-9a-f]{16,}$/i.test(s)) return true;
    if (/[.:]/.test(s)) return true;
    if (s.length > 64) return true;
    return false;
  }

  function isDynamicClass(cls) {
    const s = String(cls || '');
    if (!s) return true;
    if (STATE_CLASSES.has(s.toLowerCase())) return true;
    if (/^(is|has|js)-/.test(s)) return true;
    if (/^(css|sc|styles|makeStyles|jsx|emotion|styled|hash)[_-]?[0-9a-z_-]{4,}$/i.test(s)) return true;
    if (/^[0-9a-f]{6,10}$/i.test(s)) return true;
    if (/\d{5,}/.test(s)) return true;
    if (/^[A-Za-z]+[0-9]{2,}$/.test(s)) return true;  // e.g. mt12, col6xx
    return false;
  }

  function looksDynamicValue(v) {
    const s = String(v == null ? '' : v);
    return /\d{6,}/.test(s) || /[0-9a-f]{8}-[0-9a-f]{4}-/i.test(s) || /^[0-9a-f]{16,}$/i.test(s);
  }

  function cssString(s) {
    return String(s == null ? '' : s)
      .replace(/\\/g, '\\\\')
      .replace(/"/g, '\\"')
      .replace(/\n/g, '\\A ')
      .replace(/\r/g, '\\D ')
      .replace(/\f/g, '\\C ');
  }

  function cssIdent(s) {
    const v = String(s == null ? '' : s);
    if (/^-?[a-zA-Z_][a-zA-Z0-9_-]*$/.test(v)) return v;
    return v.replace(/[^a-zA-Z0-9_-]/g, function (c) {
      return '\\' + c.charCodeAt(0).toString(16) + ' ';
    });
  }

  // XPath 1.0 string literal, safe for quotes of either kind.
  function xpathLiteral(s) {
    const v = String(s == null ? '' : s);
    if (v.indexOf("'") === -1) return "'" + v + "'";
    if (v.indexOf('"') === -1) return '"' + v + '"';
    return 'concat(' + v.split("'").map(function (p) { return "'" + p + "'"; }).join(',"\'",') + ')';
  }

  function attrSelector(key, value) {
    return '[' + key + '="' + cssString(value) + '"]';
  }

  function nthOfType(el) {
    const tag = String(el.tagName || '*').toLowerCase();
    const parent = el.parentElement;
    if (!parent || !parent.children) return tag;
    const same = Array.prototype.filter.call(parent.children, function (c) {
      return c.tagName === el.tagName;
    });
    return tag + ':nth-of-type(' + (Array.prototype.indexOf.call(same, el) + 1) + ')';
  }

  function pathSelector(el, maxDepth) {
    const parts = [];
    let cur = el;
    const limit = maxDepth || 6;
    while (cur && cur.nodeType === 1 && parts.length < limit) {
      parts.unshift(nthOfType(cur));
      cur = cur.parentElement;
    }
    return parts.join(' > ');
  }

  function generateCandidates(info) {
    const out = [];
    const push = function (type, value, score, reason) {
      if (!value || value.length > 2048) return;
      out.push({ type: type, value: value, score: score, reason: reason });
    };

    const tag = String(info.tag || '*').toLowerCase();
    const id = String(info.id || '');
    const attrs = info.attrs || {};
    const text = String(info.text || '').replace(/\s+/g, ' ').trim();
    const usable = function (v) {
      return v != null && v !== '' && String(v).length <= 120 && !looksDynamicValue(v);
    };

    // 1. Stable, human-authored id — the strongest signal.
    if (id && !isDynamicId(id)) {
      if (/^-?[a-zA-Z_][a-zA-Z0-9_-]*$/.test(id)) push('css', '#' + id, 96, 'stable-id');
      push('css', attrSelector('id', id), 95, 'stable-id-attr');
      push('xpath', '//*[@id=' + xpathLiteral(id) + ']', 92, 'stable-id-xpath');
    }

    // 2. Dedicated test attributes (highest priority after a stable id).
    for (const item of TEST_ATTRS) {
      const v = attrs[item.key];
      if (!usable(v)) continue;
      push('css', tag + attrSelector(item.key, v), item.score, 'test-attr');
      push('css', attrSelector(item.key, v), item.score - 2, 'test-attr');
      push('xpath', '//' + tag + '[@' + item.key + '=' + xpathLiteral(v) + ']', item.score - 8, 'test-attr-xpath');
      break;
    }

    // 3. Semantic form / accessibility attributes.
    if (usable(attrs.name)) {
      const n = attrs.name;
      push('css', tag + attrSelector('name', n), 86, 'name');
      if (usable(attrs.type)) {
        push('css', tag + attrSelector('name', n) + attrSelector('type', attrs.type), 90, 'name+type');
      }
      push('xpath', '//' + tag + '[@name=' + xpathLiteral(n) + ']', 82, 'name-xpath');
    }
    if (usable(attrs['aria-label'])) {
      push('css', tag + attrSelector('aria-label', attrs['aria-label']), 84, 'aria-label');
      push('xpath', '//' + tag + '[@aria-label=' + xpathLiteral(attrs['aria-label']) + ']', 78, 'aria-label-xpath');
    }
    if (usable(attrs.placeholder)) push('css', tag + attrSelector('placeholder', attrs.placeholder), 80, 'placeholder');
    if (usable(attrs.role)) push('css', tag + attrSelector('role', attrs.role), 72, 'role');
    if (usable(attrs.title)) push('css', tag + attrSelector('title', attrs.title), 68, 'title');
    if (usable(attrs.href)) push('css', tag + attrSelector('href', attrs.href), 62, 'href');
    if (usable(attrs.alt)) push('css', tag + attrSelector('alt', attrs.alt), 66, 'alt');

    // 4. Stable, semantic classes (generated hashes and state classes excluded).
    const stableClasses = (info.classes || []).filter(function (c) { return !isDynamicClass(c); });
    if (stableClasses.length) {
      const picked = stableClasses.slice(0, 3);
      push('css', tag + picked.map(function (c) { return '.' + cssIdent(c); }).join(''), 70, 'stable-class');
      if (picked.length > 1) push('css', tag + '.' + cssIdent(picked[0]), 64, 'stable-class');
      push('css', '.' + cssIdent(picked[0]), 58, 'stable-class');
      push('xpath', "//" + tag + "[contains(concat(' ', normalize-space(@class), ' '), ' " + picked[0] + " ')]",
        56, 'class-xpath');
    }

    // 5. Exact visible text (short, whitespace-normalized).
    if (text && text.length <= 80) {
      push('xpath', '//' + tag + '[normalize-space(.)=' + xpathLiteral(text) + ']', 74, 'exact-text');
      push('xpath', '//' + tag + '[normalize-space(text())=' + xpathLiteral(text) + ']', 70, 'own-text');
    }

    // 6. Structural fallbacks — always present so a descriptor is never empty.
    if (info.path) push('css', info.path, 32, 'dom-path');
    if (Number(info.indexAmongSameTag) > 0) {
      push('css', tag + ':nth-of-type(' + Number(info.indexAmongSameTag) + ')', 20, 'nth-of-type');
    }
    push('css', tag, 10, 'tag-only');

    const seen = new Set();
    return out
      .filter(function (c) {
        const key = c.type + '\u0000' + c.value;
        if (seen.has(key)) return false;
        seen.add(key);
        return true;
      })
      .sort(function (a, b) { return b.score - a.score; })
      .slice(0, 12);
  }

  function describeElement(el) {
    const tag = String(el.tagName || '*').toLowerCase();
    const attrs = {};
    for (const key of ATTR_KEYS) {
      const v = el.getAttribute ? el.getAttribute(key) : null;
      if (v != null && v !== '' && String(v).length <= 200) attrs[key] = String(v);
    }
    const classes = el.classList
      ? Array.prototype.slice.call(el.classList)
      : String(el.className || '').split(/\s+/);
    const text = String(el.innerText || el.textContent || '').replace(/\s+/g, ' ').trim().slice(0, 200);
    const ownText = Array.prototype.filter.call(el.childNodes || [], function (n) {
      return n.nodeType === 3;
    }).map(function (n) { return n.nodeValue; }).join('').replace(/\s+/g, ' ').trim().slice(0, 200);

    let indexAmongSameTag = 0;
    const parent = el.parentElement;
    if (parent && parent.children) {
      const same = Array.prototype.filter.call(parent.children, function (c) { return c.tagName === el.tagName; });
      indexAmongSameTag = Array.prototype.indexOf.call(same, el) + 1;
    }

    let textIndex = 0;
    const root = el.getRootNode ? el.getRootNode() : el.ownerDocument;
    if (root && root.querySelectorAll) {
      const pool = root.querySelectorAll(tag);
      let seen = 0;
      for (const node of pool) {
        const t = String(node.innerText || node.textContent || '').replace(/\s+/g, ' ').trim();
        if (t === text) {
          if (node === el) { textIndex = seen; break; }
          seen += 1;
        }
      }
    }

    return {
      tag: tag,
      id: String(el.id || ''),
      classes: classes.filter(Boolean),
      attrs: attrs,
      text: text,
      ownText: ownText,
      indexAmongSameTag: indexAmongSameTag,
      textIndex: textIndex,
      path: pathSelector(el),
    };
  }

  function collectFramePath(el) {
    const path = [];
    try {
      let win = el.ownerDocument && el.ownerDocument.defaultView;
      let guard = 0;
      while (win && win !== win.top && guard++ < 8) {
        const frameEl = win.frameElement;
        if (!frameEl) break;
        const parentDoc = frameEl.ownerDocument;
        const frames = parentDoc.querySelectorAll('iframe,frame');
        const idx = Array.prototype.indexOf.call(frames, frameEl);
        if (idx < 0) break;
        path.unshift(idx);
        win = parentDoc.defaultView;
      }
    } catch (e) { /* cross-origin: CDP frame context will handle it */ }
    return path;
  }

  function bestHostSelector(host) {
    const id = String(host.id || '');
    if (id && !isDynamicId(id) && /^-?[a-zA-Z_][a-zA-Z0-9_-]*$/.test(id)) return '#' + id;
    for (const key of ['data-testid', 'data-test', 'data-qa', 'data-cy']) {
      const v = host.getAttribute && host.getAttribute(key);
      if (v && !looksDynamicValue(v)) return attrSelector(key, v);
    }
    return pathSelector(host);
  }

  function collectShadowHosts(el) {
    const hosts = [];
    let node = el;
    let guard = 0;
    while (node && guard++ < 8) {
      const r = node.getRootNode ? node.getRootNode() : null;
      if (!r || !r.host) break;
      hosts.unshift(bestHostSelector(r.host));
      node = r.host;
    }
    return hosts;
  }

  // Pure assembly: opts.verify(candidate) -> true when it uniquely matches.
  function buildDescriptorFromInfo(info, opts) {
    opts = opts || {};
    const all = generateCandidates(info);
    let candidates;
    if (typeof opts.verify === 'function') {
      candidates = all.filter(function (c) { return opts.verify(c) === true; })
        .slice(0, 8)
        .map(function (c) { return Object.assign({}, c, { unique: true }); });
      if (candidates.length === 0) {
        candidates = all.slice(0, 3).map(function (c) {
          return Object.assign({}, c, { unique: false });
        });
      }
    } else {
      candidates = all.slice(0, 8).map(function (c) {
        return Object.assign({}, c, { unique: null });
      });
    }
    const text = String(info.text || '').replace(/\s+/g, ' ').trim().slice(0, 120);
    return {
      version: 1,
      candidates: candidates,
      framePath: opts.framePath || [],
      shadowHosts: opts.shadowHosts || [],
      fallback: {
        tag: info.tag || '*',
        text: text,
        index: Number(info.textIndex) || 0,
      },
      meta: {
        tag: info.tag || '',
        id: String(info.id || ''),
        text: text,
        url: opts.url || '',
        title: opts.title || '',
        path: info.path || '',
        ts: opts.now || Date.now(),
      },
    };
  }

  function buildDescriptor(el, opts) {
    opts = opts || {};
    const info = describeElement(el);
    const root = el.getRootNode ? el.getRootNode() : (el.ownerDocument || document);
    const verify = function (c) {
      try {
        let nodes;
        if (c.type === 'css') {
          nodes = Array.prototype.slice.call(root.querySelectorAll(c.value));
        } else {
          const doc = root.ownerDocument || (root.nodeType === 9 ? root : document);
          const result = doc.evaluate(c.value, root, null, 7, null);  // ORDERED_NODE_SNAPSHOT_TYPE
          nodes = [];
          for (let i = 0; i < result.snapshotLength; i++) nodes.push(result.snapshotItem(i));
        }
        return nodes.length === 1 && nodes[0] === el;
      } catch (e) {
        return false;
      }
    };
    return buildDescriptorFromInfo(info, Object.assign({}, opts, {
      framePath: collectFramePath(el),
      shadowHosts: collectShadowHosts(el),
      verify: verify,
      url: typeof location !== 'undefined' ? location.href : '',
      title: typeof document !== 'undefined' ? document.title : '',
    }));
  }

  return {
    ATTR_KEYS: ATTR_KEYS,
    TEST_ATTRS: TEST_ATTRS,
    isDynamicId: isDynamicId,
    isDynamicClass: isDynamicClass,
    looksDynamicValue: looksDynamicValue,
    cssString: cssString,
    cssIdent: cssIdent,
    xpathLiteral: xpathLiteral,
    pathSelector: pathSelector,
    generateCandidates: generateCandidates,
    describeElement: describeElement,
    buildDescriptorFromInfo: buildDescriptorFromInfo,
    buildDescriptor: buildDescriptor,
  };
});
