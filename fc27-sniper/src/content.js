// *content.js — MAIN world: token extraction + request template capture + XHR proxy*
// Runs at document_start so we hook before EA's own scripts.

(function () {
  'use strict';

  const vault = {
    sid: null,
    phishingToken: null,
    nucleusId: null,
    route: null,
    apiBase: null,
    timestamp: null,
    // Captured from EA's own successful transfermarket request:
    requestTemplate: null,   // { headers: {}, wc: bool }
  };

  window.__FUT_SNIPER_VAULT__ = vault;

  function broadcastTokens() {
    window.postMessage({ type: '__FUT_SNIPER_TOKENS__', payload: { ...vault, requestTemplate: undefined } }, '*');
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

  // ── XHR hook ─────────────────────────────────────────────────────────────────
  const OriginalXHR = window.XMLHttpRequest;
  window.__FUT_ORIGINAL_XHR__ = OriginalXHR;

  class HookedXHR extends OriginalXHR {
    constructor() { super(); this.__h = {}; this.__wc = false; }
    set withCredentials(v) { this.__wc = v; super.withCredentials = v; }
    get withCredentials()  { return super.withCredentials; }
    setRequestHeader(n, v) { this.__h[n.toLowerCase()] = v; super.setRequestHeader(n, v); }
    open(m, url, ...r)     { this.__url = url; this.__method = m; tryDetectBase(url); super.open(m, url, ...r); }
    send(body) {
      const orig = this.onreadystatechange;
      this.onreadystatechange = (e) => {
        if (this.readyState === 4 && this.__url && this.__url.includes('fut.ea.com')) {
          extractFromHeaderMap(this.__h);
          // Capture phishing token from response
          try {
            const p = this.getResponseHeader('X-UT-PHISHING-TOKEN');
            if (p) extractFromHeaderMap({ 'x-ut-phishing-token': p });
          } catch (_) {}
          // ── Template capture: save exact headers from EA's own successful GET ──
          if (
            this.status === 200 &&
            this.__url.includes('transfermarket') &&
            (this.__method ?? 'GET').toUpperCase() === 'GET'
          ) {
            vault.requestTemplate = { headers: { ...this.__h }, wc: this.__wc };
          }
        }
        if (orig) orig.call(this, e);
      };
      extractFromHeaderMap(this.__h);
      super.send(body);
    }
  }
  window.XMLHttpRequest = HookedXHR;

  // ── Fetch hook ────────────────────────────────────────────────────────────────
  const originalFetch = window.fetch;
  window.__FUT_ORIGINAL_FETCH__ = originalFetch;

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

  // ── Message handlers ──────────────────────────────────────────────────────────
  window.addEventListener('message', (event) => {
    if (event.source !== window) return;

    // Token request
    if (event.data?.type === '__FUT_SNIPER_REQUEST_TOKENS__') {
      broadcastTokens();
      return;
    }

    // Proxy request — uses captured EA header template so CORS matches exactly
    if (event.data?.type === '__FUT_PROXY_REQUEST__') {
      const { reqId, url, method, body, overrideHeaders } = event.data;

      const tmpl = vault.requestTemplate;
      if (!tmpl) {
        window.postMessage({
          type: '__FUT_PROXY_RESPONSE__', reqId,
          ok: false, status: 0, body: '',
          error: 'NO_TEMPLATE — open Transfer Market and do one manual search first!',
        }, '*');
        return;
      }

      const xhr = new OriginalXHR();
      xhr.open(method ?? 'GET', url, true);
      xhr.withCredentials = tmpl.wc;
      xhr.timeout = 12000;

      // Apply captured headers from EA's own successful request
      for (const [k, v] of Object.entries(tmpl.headers)) {
        try { xhr.setRequestHeader(k, v); } catch (_) {}
      }

      // Override/add specific headers for this request
      if (overrideHeaders) {
        for (const [k, v] of Object.entries(overrideHeaders)) {
          try { xhr.setRequestHeader(k, v); } catch (_) {}
        }
      }

      // Always set fresh phishing token
      if (vault.phishingToken) {
        try { xhr.setRequestHeader('X-UT-PHISHING-TOKEN', vault.phishingToken); } catch (_) {}
      }

      xhr.onload = function () {
        const newPT = this.getResponseHeader('X-UT-PHISHING-TOKEN');
        if (newPT && vault.phishingToken !== newPT) {
          vault.phishingToken = newPT;
          broadcastTokens();
        }
        window.postMessage({
          type: '__FUT_PROXY_RESPONSE__', reqId,
          ok:     this.status >= 200 && this.status < 300,
          status: this.status,
          body:   this.responseText,
          phishingToken: newPT ?? null,
        }, '*');
      };
      xhr.onerror   = () => window.postMessage({ type: '__FUT_PROXY_RESPONSE__', reqId, ok: false, status: 0, body: '', error: 'XHR error — CORS or network' }, '*');
      xhr.ontimeout = () => window.postMessage({ type: '__FUT_PROXY_RESPONSE__', reqId, ok: false, status: 0, body: '', error: 'XHR timeout' }, '*');
      xhr.send(body ?? null);
    }
  });

  window.postMessage({ type: '__FUT_SNIPER_READY__' }, '*');
})();
