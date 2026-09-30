// *injector.js — ISOLATED world: bidirectional bridge content.js <-> background.js*

'use strict';

const pendingProxies = new Map();

// ── content.js → background.js ───────────────────────────────────────────────
window.addEventListener('message', (event) => {
  if (event.source !== window) return;

  if (event.data?.type === '__FUT_SNIPER_TOKENS__') {
    const payload = event.data.payload;
    if (payload?.sid || payload?.phishingToken) {
      chrome.runtime.sendMessage({ type: 'FUT_TOKENS_RELAY', payload }).catch(() => {});
    }
  }

  if (event.data?.type === '__FUT_SNIPER_READY__') {
    window.postMessage({ type: '__FUT_SNIPER_REQUEST_TOKENS__' }, '*');
  }

  // Proxy response: relay back to whichever proxyFetch call is waiting
  if (event.data?.type === '__FUT_PROXY_RESPONSE__') {
    const cb = pendingProxies.get(event.data.reqId);
    if (cb) {
      pendingProxies.delete(event.data.reqId);
      cb(event.data);
    }
  }
});

// ── background.js → content.js ───────────────────────────────────────────────
chrome.runtime.onMessage.addListener((msg, _sender, sendResponse) => {
  if (msg.type !== 'FUT_PROXY_REQUEST') return false;

  const reqId = Math.random().toString(36).slice(2) + Date.now().toString(36);

  const timer = setTimeout(() => {
    if (pendingProxies.has(reqId)) {
      pendingProxies.delete(reqId);
      sendResponse({ ok: false, status: 0, body: '', error: 'injector proxy timeout' });
    }
  }, 15000);

  pendingProxies.set(reqId, (resp) => {
    clearTimeout(timer);
    sendResponse(resp);
  });

  // Explicit fields — never spread msg which would clobber type
  window.postMessage({
    type:            '__FUT_PROXY_REQUEST__',
    reqId,
    url:             msg.url,
    method:          msg.method,
    body:            msg.body,
    overrideHeaders: msg.overrideHeaders,
  }, '*');

  return true; // async sendResponse
});
