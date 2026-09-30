// *background.js — service worker: snipe loop via content.js template-replay proxy*
// All FUT HTTP goes through content.js MAIN world using EA's own captured request headers,
// so CORS and credentials match exactly what EA's own requests use.

'use strict';

let sniperState = {
  running:      false,
  config:       null,
  auth:         null,
  stats:        { searches: 0, buys: 0, coinsSaved: 0 },
  loopTimer:    null,
  detectedBase: null,
};

const FUT_BASE_FALLBACK = 'https://utas.external.s2.fut.ea.com/ut/game/fc27';

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }
function futTax(c)  { return Math.floor(c * 0.95); }

function getFutBase() {
  return sniperState.detectedBase ?? FUT_BASE_FALLBACK;
}

function log(type, msg) {
  chrome.runtime.sendMessage({ type: 'LOG', payload: { type, msg, ts: Date.now() } }).catch(() => {});
}
function updateStats() {
  chrome.runtime.sendMessage({ type: 'STATS', payload: { ...sniperState.stats } }).catch(() => {});
}

async function getFutTabId() {
  const tabs = await chrome.tabs.query({
    url: [
      'https://www.ea.com/ea-sports-fc/ultimate-team/web-app/*',
      'https://www.ea.com/fifa/ultimate-team/web-app/*',
    ],
  });
  return tabs.length > 0 ? tabs[0].id : null;
}

// Read vault directly via executeScript (MAIN world) — fast token sync
async function syncTokensFromTab(tabId) {
  try {
    const r = await chrome.scripting.executeScript({
      target: { tabId, allFrames: false },
      world:  'MAIN',
      func:   () => {
        const v = window.__FUT_SNIPER_VAULT__;
        if (!v) return null;
        return {
          sid:             v.sid,
          phishingToken:   v.phishingToken,
          nucleusId:       v.nucleusId,
          route:           v.route,
          apiBase:         v.apiBase,
          hasTemplate:     !!v.requestTemplate,
        };
      },
    });
    const vault = r?.[0]?.result;
    if (!vault) return;
    if (vault.apiBase && !sniperState.detectedBase) {
      sniperState.detectedBase = vault.apiBase;
      log('info', `API base auto-detected: ${vault.apiBase}`);
    }
    if (vault.sid) {
      sniperState.auth = {
        sid:           vault.sid,
        phishingToken: vault.phishingToken ?? sniperState.auth?.phishingToken,
        nucleusId:     vault.nucleusId     ?? sniperState.auth?.nucleusId,
        route:         vault.route         ?? sniperState.auth?.route,
      };
    }
    return vault;
  } catch (_) {
    return null;
  }
}

// ── Core proxy: sends request through injector.js → content.js MAIN world ────
// content.js uses EA's captured request template so CORS headers match exactly.
function proxyFetch(url, method = 'GET', body = null, overrideHeaders = null) {
  return new Promise(async (resolve, reject) => {
    const tabId = await getFutTabId();
    if (!tabId) { reject(new Error('NO_FUT_TAB')); return; }

    const vault = await syncTokensFromTab(tabId);

    if (!sniperState.auth?.sid) {
      reject(new Error('NO_TOKENS — do a manual FUT search first!'));
      return;
    }
    if (!vault?.hasTemplate) {
      reject(new Error('NO_TEMPLATE — open Transfer Market and do one manual search first!'));
      return;
    }

    // Extra headers that override template values for this specific call
    const hdrs = {
      'X-UT-PHISHING-TOKEN': sniperState.auth.phishingToken ?? '',
      'X-UT-SID':            sniperState.auth.sid,
      'Easw-Session-Data-Nucleus-Id': sniperState.auth.nucleusId ?? '',
      ...(sniperState.auth.route ? { 'X-UT-Route': sniperState.auth.route } : {}),
      ...(body ? { 'Content-Type': 'application/json' } : {}),
      ...(overrideHeaders ?? {}),
    };

    chrome.tabs.sendMessage(
      tabId,
      { type: 'FUT_PROXY_REQUEST', url, method, body, overrideHeaders: hdrs },
      (resp) => {
        if (chrome.runtime.lastError) {
          reject(new Error('PROXY_FAIL: ' + chrome.runtime.lastError.message));
          return;
        }
        if (!resp) { reject(new Error('PROXY_NO_RESP')); return; }
        if (resp.error) { reject(new Error(resp.error)); return; }
        // Refresh phishing token from response
        if (resp.phishingToken && sniperState.auth) {
          sniperState.auth.phishingToken = resp.phishingToken;
        }
        resolve(resp);
      },
    );
  });
}

// ── FUT API helpers ────────────────────────────────────────────────────────────
function buildSearchURL(config) {
  const p = new URLSearchParams();
  p.set('type',  'player');
  p.set('num',   '21');
  p.set('start', '0');
  if (config.playerName) p.set('maskedDefId', config.playerName);
  if (config.position)   p.set('pos',  config.position);
  if (config.quality)    p.set('lev',  config.quality);
  if (config.maxBuyNow)  p.set('maxb', String(config.maxBuyNow));
  if (config.maxBid)     p.set('maxc', String(config.maxBid));
  return `${getFutBase()}/transfermarket?${p.toString()}`;
}

async function searchMarket(config) {
  const url  = buildSearchURL(config);
  const resp = await proxyFetch(url, 'GET');
  if (resp.status === 401 || resp.status === 403) throw new Error(`AUTH_EXPIRED:${resp.status}`);
  if (resp.status === 429) throw new Error('RATE_LIMITED');
  if (!resp.ok) throw new Error(`HTTP:${resp.status} body=${resp.body?.slice(0, 80)}`);
  return JSON.parse(resp.body).auctionInfo ?? [];
}

async function buyNow(tradeId, price) {
  const resp = await proxyFetch(
    `${getFutBase()}/trade/${tradeId}/bid`, 'PUT',
    JSON.stringify({ bid: price }),
  );
  if (resp.status === 401 || resp.status === 403) throw new Error(`AUTH_EXPIRED:${resp.status}`);
  return { ok: resp.ok, status: resp.status };
}

async function placeBid(tradeId, amount) {
  const resp = await proxyFetch(
    `${getFutBase()}/trade/${tradeId}/bid`, 'PUT',
    JSON.stringify({ bid: amount }),
  );
  return { ok: resp.ok, status: resp.status };
}

function roundToBracket(n) {
  if (n < 1000)    return Math.ceil(n / 50)   * 50;
  if (n < 10000)   return Math.ceil(n / 100)  * 100;
  if (n < 50000)   return Math.ceil(n / 250)  * 250;
  if (n < 100000)  return Math.ceil(n / 500)  * 500;
  if (n < 1000000) return Math.ceil(n / 1000) * 1000;
  return Math.ceil(n / 5000) * 5000;
}

async function processResults(auctions, config) {
  for (const auction of auctions) {
    const { buyNowPrice, currentBid, tradeId, expires } = auction;
    if (expires === -1) continue;

    if (
      (config.autoBuyMode === 'buynow' || config.autoBuyMode === 'both') &&
      buyNowPrice > 0 && buyNowPrice <= config.maxBuyNow
    ) {
      const profit = config.marketValue ? futTax(config.marketValue) - buyNowPrice : 0;
      if (config.minProfit > 0 && profit < config.minProfit && config.marketValue) continue;

      log('info', `BN attempt: ${auction.itemData?.lastName ?? 'player'} @ ${buyNowPrice}c`);
      const result = await buyNow(tradeId, buyNowPrice);
      if (result.ok) {
        sniperState.stats.buys++;
        sniperState.stats.coinsSaved += (config.maxBuyNow - buyNowPrice);
        updateStats();
        log('buy', `BOUGHT @ ${buyNowPrice}c — saved ${config.maxBuyNow - buyNowPrice}c`);
        return true;
      } else {
        log('miss', `Buy failed [${result.status}]`);
      }
    }

    if (
      (config.autoBuyMode === 'bid' || config.autoBuyMode === 'both') &&
      config.maxBid > 0 && expires > 5
    ) {
      const nextBid = roundToBracket(currentBid + 50);
      if (nextBid <= config.maxBid) {
        const r = await placeBid(tradeId, nextBid);
        if (r.ok) log('buy', `BID ${nextBid}c on ${tradeId}`);
      }
    }
  }
  return false;
}

async function sniperLoop() {
  if (!sniperState.running) return;
  const { config } = sniperState;
  const delay = Math.max(200, config.interval + Math.floor(Math.random() * 100) - 50);

  try {
    sniperState.stats.searches++;
    updateStats();
    const auctions = await searchMarket(config);
    if (auctions.length > 0) {
      log('info', `[${sniperState.stats.searches}] Found ${auctions.length} results`);
      await processResults(auctions, config);
    } else {
      if (sniperState.stats.searches % 10 === 0) {
        log('info', `[${sniperState.stats.searches}] No listings found`);
      }
    }
  } catch (err) {
    const msg = err.message ?? String(err);
    if (msg.startsWith('AUTH_EXPIRED')) {
      log('err', 'Session expired — refresh FUT Web App and do a manual search');
      sniperState.running = false;
      chrome.runtime.sendMessage({ type: 'STOPPED', reason: 'auth_expired' }).catch(() => {});
      return;
    }
    if (msg === 'RATE_LIMITED')     { log('err', 'Rate limited — backoff 10s'); await sleep(10000); }
    else if (msg === 'NO_FUT_TAB')  { log('err', 'FUT tab not found — keep it open!'); await sleep(3000); }
    else if (msg.startsWith('NO_TEMPLATE')) {
      log('err', 'Go to Transfer Market → search once manually, then sniper will work');
      await sleep(5000);
    }
    else { log('err', `Error: ${msg}`); await sleep(2000); }
  }

  if (sniperState.running) sniperState.loopTimer = setTimeout(sniperLoop, delay);
}

// ── Message listener ───────────────────────────────────────────────────────────
chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  if (msg.type === 'FUT_TOKENS_RELAY') {
    const t = msg.payload;
    sniperState.auth = {
      sid:           t.sid           ?? sniperState.auth?.sid,
      phishingToken: t.phishingToken ?? sniperState.auth?.phishingToken,
      nucleusId:     t.nucleusId     ?? sniperState.auth?.nucleusId,
      route:         t.route         ?? sniperState.auth?.route,
    };
    if (t.apiBase && !sniperState.detectedBase) {
      sniperState.detectedBase = t.apiBase;
      log('info', `API base auto-detected: ${t.apiBase}`);
    }
    chrome.storage.session.set({ futAuth: sniperState.auth });
    return;
  }

  switch (msg.type) {
    case 'START': {
      if (sniperState.running) { sendResponse({ ok: false, reason: 'already_running' }); break; }
      sniperState.config = msg.config;
      sniperState.stats  = { searches: 0, buys: 0, coinsSaved: 0 };
      sniperState.running = true;
      getFutTabId().then(async (tabId) => {
        if (!tabId) {
          log('err', 'No FUT Web App tab — open it first!');
          sniperState.running = false;
          sendResponse({ ok: false, reason: 'no_fut_tab' });
          return;
        }
        const vault = await syncTokensFromTab(tabId);
        const base     = getFutBase();
        const tokenOk  = !!sniperState.auth?.sid;
        const tmplOk   = !!vault?.hasTemplate;
        log('info', `Sniper started | base=${base} | auth=${tokenOk ? 'OK' : 'MISSING'} | template=${tmplOk ? 'OK' : 'MISSING — do a manual TM search!'} | max=${msg.config.maxBuyNow}c | ${msg.config.interval}ms`);
        sniperLoop();
        sendResponse({ ok: true });
      });
      return true;
    }
    case 'STOP': {
      sniperState.running = false;
      if (sniperState.loopTimer) clearTimeout(sniperState.loopTimer);
      log('info', 'Sniper stopped');
      sendResponse({ ok: true });
      break;
    }
    case 'GET_STATUS': {
      sendResponse({
        running:  sniperState.running,
        hasAuth:  !!sniperState.auth?.sid,
        apiBase:  sniperState.detectedBase ?? FUT_BASE_FALLBACK,
        stats:    sniperState.stats,
      });
      break;
    }
  }
});
