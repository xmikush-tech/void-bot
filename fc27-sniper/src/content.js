// *content.js — runs in MAIN world to intercept XHR/fetch and extract FUT auth tokens*
// Hooks XMLHttpRequest and fetch before EA's app code runs, stealing session headers.

(function () {
  'use strict';

  const vault = {
    sid: null,
    phishingToken: null,
    nucleusId: null,
    platform: null,
    route: null,
    timestamp: null,
  };

  function broadcastTokens() {
    window.postMessage({ type: '__FUT_SNIPER_TOKENS__', payload: { ...vault } }, '*');
  }

  function extractFromHeaderMap(headerMap) {
    let updated = false;

    const sid = headerMap['x-ut-sid'];
    if (sid && sid !== vault.sid) { vault.sid = sid; updated = true; }

    const pt = headerMap['x-ut-phishing-token'];
    if (pt && pt !== vault.phishingToken) { vault.phishingToken = pt; updated = true; }

    const nid = headerMap['easw-session-data-nucleus-id'];
    if (nid && nid !== vault.nucleusId) { vault.nucleusId = nid; updated = true; }

    const route = headerMap['x-ut-route'];
    if (route && route !== vault.route) { vault.route = route; updated = true; }

    if (updated) {
      vault.timestamp = Date.now();
      broadcastTokens();
    }
  }

  const OriginalXHR = window.XMLHttpRequest;

  class HookedXHR extends OriginalXHR {
    constructor() {
      super();
      this.__capturedHeaders = {};
    }

    setRequestHeader(name, value) {
      this.__capturedHeaders[name.toLowerCase()] = value;
      super.setRequestHeader(name, value);
    }

    open(method, url, ...rest) {
      this.__url = url;
      super.open(method, url, ...rest);
    }

    send(body) {
      const originalOnRS = this.onreadystatechange;
      this.onreadystatechange = (e) => {
        if (this.readyState === 4 && this.__url && this.__url.includes('fut.ea.com')) {
          extractFromHeaderMap(this.__capturedHeaders);
          try {
            const respPT = this.getResponseHeader('X-UT-PHISHING-TOKEN');
            if (respPT) extractFromHeaderMap({ 'x-ut-phishing-token': respPT });
          } catch (_) {}
        }
        if (originalOnRS) originalOnRS.call(this, e);
      };
      extractFromHeaderMap(this.__capturedHeaders);
      super.send(body);
    }
  }

  window.XMLHttpRequest = HookedXHR;

  const originalFetch = window.fetch;

  window.fetch = async function (input, init = {}) {
    const url = typeof input === 'string' ? input : input?.url ?? '';

    if (url.includes('fut.ea.com')) {
      const headers = init.headers ?? {};
      const headerMap = {};

      if (headers instanceof Headers) {
        headers.forEach((v, k) => { headerMap[k.toLowerCase()] = v; });
      } else {
        for (const k in headers) headerMap[k.toLowerCase()] = headers[k];
      }

      extractFromHeaderMap(headerMap);
    }

    const response = await originalFetch.call(this, input, init);

    if (url.includes('fut.ea.com')) {
      try {
        const pt = response.headers.get('X-UT-PHISHING-TOKEN');
        if (pt) extractFromHeaderMap({ 'x-ut-phishing-token': pt });
      } catch (_) {}
    }

    return response;
  };

  window.addEventListener('message', (event) => {
    if (event.source !== window) return;
    if (event.data?.type === '__FUT_SNIPER_REQUEST_TOKENS__') {
      broadcastTokens();
    }
  });

  window.postMessage({ type: '__FUT_SNIPER_READY__' }, '*');
})();
