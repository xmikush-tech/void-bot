// *injector.js — ISOLATED world bridge: postMessage <-> chrome.runtime relay*
// Bidirectional: tokens content->background, proxy requests background->content->background.

'use strict';

// content -> background: token relay + proxy response relay
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

  if (event.data?.type === '__FUT_PROXY_RESPONSE__') {
    chrome.runtime.sendMessage({ type: 'FUT_PROXY_RESPONSE', payload: event.data }).catch(() => {});
  }
});

// background -> content: forward proxy requests into page
chrome.runtime.onMessage.addListener((msg) => {
  if (msg.type === 'FUT_PROXY_REQUEST') {
    window.postMessage({ type: '__FUT_PROXY_REQUEST__', ...msg.payload }, '*');
  }
});
