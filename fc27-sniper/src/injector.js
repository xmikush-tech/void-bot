// *injector.js — isolated-world bridge: listens to postMessage from content.js (MAIN world)*
// Relays FUT auth tokens to the service worker via chrome.runtime.sendMessage.

'use strict';

window.addEventListener('message', (event) => {
  if (event.source !== window) return;

  if (event.data?.type === '__FUT_SNIPER_TOKENS__') {
    const payload = event.data.payload;
    if (payload?.sid || payload?.phishingToken) {
      chrome.runtime.sendMessage({
        type: 'FUT_TOKENS_RELAY',
        payload,
      }).catch(() => {});
    }
  }

  if (event.data?.type === '__FUT_SNIPER_READY__') {
    window.postMessage({ type: '__FUT_SNIPER_REQUEST_TOKENS__' }, '*');
  }
});
