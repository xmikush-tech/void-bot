// *content.js — MAIN world: token extraction + fetch proxy for background.js*
// Runs on www.ea.com so all fetch calls carry the correct Origin header.

(function () {
  'use strict';

  const vault = {
    sid: null,
    phishingToken: null,
    nucleusId: null,
    route: null,
    apiBase: null,
    timestamp: null,
  };

  window.__FUT_SNIPER_VAULT__ = vault;

  function broadcastTokens() {
    window.postMessage({ type: '__FUT_SNIPER_TOKENS__', payload: { ...vault } }, '*');
  }

  function tryDetectBase(url) {
    if (!vault.apiBase && url && url.includes('fut.ea.com') && url.includes('/ut/game/')) {
      const m = url.match(/(https:\/\/[^\/]+\/ut\/game\/[^\/]+)/);
      if (m) { vault.apiBase = m[1]; broadcastTokens(); }
    }
  }

  function extractFromHeaderMap(headerMap) {
    let updated = false;
    const sid   = headerMap['x-ut-sid'];
    const pt    = headerMap['x-ut-phishing-token'];
    const nid   = headerMap['easw-session-data-nucleus-id'];
    const route = headerMap['x-ut-route'];
    if (sid   && sid   !== vault.sid)          { vault.sid = sid;            updated = true; }
    if (pt    && pt    !== vault.phishingToken) { vault.phishingToken = pt;   updated = true; }
    if (nid   && nid   !== vault.nucleusId)    { vault.nucleusId = nid;       updated = true; }
    if (route && route !== vault.route)        { vault.route = route;         updated = true; }
    if (updated) { vault.timestamp = Date.now(); broadcastTokens(); }
  }

  // ── XHR hook ────────────────────────────────────────────────────────────────
  const OriginalXHR = window.XMLHttpRequest;
  window.__FUT_ORIGINAL_XHR__ = OriginalXHR;  // exposed for executeScript bypass

  class HookedXHR extends OriginalXHR {
    constructor() { super(); this.__h = {}; }
    setRequestHeader(n, v) { this.__h[n.toLowerCase()] = v; super.setRequestHeader(n, v); }
    open(m, url, ...r) { this.__url = url; tryDetectBase(url); super.open(m, url, ...r); }
    send(body) {
      const orig = this.onreadystatechange;
      this.onreadystatechange = (e) => {
        if (this.readyState === 4 && this.__url && this.__url.includes('fut.ea.com')) {
          extractFromHeaderMap(this.__h);
          try {
            const p = this.getResponseHeader('X-UT-PHISHING-TOKEN');
            if (p) extractFromHeaderMap({ 'x-ut-phishing-token': p });
          } catch (_) {}
        }
        if (orig) orig.call(this, e);
      };
      extractFromHeaderMap(this.__h);
      super.send(body);
    }
  }
  window.XMLHttpRequest = HookedXHR;

  // ── Fetch hook ───────────────────────────────────────────────────────────────
  const originalFetch = window.fetch;
  window.__FUT_ORIGINAL_FETCH__ = originalFetch;  // exposed for executeScript bypass

  window.fetch = async function (input, init = {}) {
    const url = typeof input === 'string' ? input : input?.url ?? '';
    if (url.includes('fut.ea.com')) {
      tryDetectBase(url);
      const hm = {};
      const h = init.headers ?? {};
      if (h instanceof Headers) h.forEach((v, k) => { hm[k.toLowerCase()] = v; });
      else for (const k in h) hm[k.toLowerCase()] = h[k];
      extractFromHeaderMap(hm);
    }
    const resp = await originalFetch.call(this, input, init);
    if (url.includes('fut.ea.com')) {
      try {
        const p = resp.headers.get('X-UT-PHISHING-TOKEN');
        if (p) extractFromHeaderMap({ 'x-ut-phishing-token': p });
      } catch (_) {}
    }
    return resp;
  };

  // ── Token relay ──────────────────────────────────────────────────────────────
  window.addEventListener('message', (event) => {
    if (event.source !== window) return;
    if (event.data?.type === '__FUT_SNIPER_REQUEST_TOKENS__') broadcastTokens();
  });

  window.postMessage({ type: '__FUT_SNIPER_READY__' }, '*');
})();
