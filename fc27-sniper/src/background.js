// *background.js — service worker: orchestrates the snipe loop, owns all FUT API calls*
// All coin arithmetic is integer. EA FUT tax = 5%, floor division.

'use strict';

let sniperState = {
  running: false,
  config: null,
  auth: null,
  stats: { searches: 0, buys: 0, coinsSaved: 0 },
  loopTimer: null,
};

const FUT_BASE = 'https://utas.external.s2.fut.ea.com/ut/game/fc27';

function sleep(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

function futTax(coins) {
  return Math.floor(coins * 0.95);
}

function log(type, msg) {
  chrome.runtime.sendMessage({ type: 'LOG', payload: { type, msg, ts: Date.now() } }).catch(() => {});
}

function updateStats() {
  chrome.runtime.sendMessage({ type: 'STATS', payload: { ...sniperState.stats } }).catch(() => {});
}

function buildHeaders(auth) {
  return {
    'Content-Type': 'application/json',
    'Accept': 'application/json',
    'X-UT-SID': auth.sid,
    'X-UT-PHISHING-TOKEN': auth.phishingToken,
    'Easw-Session-Data-Nucleus-Id': auth.nucleusId,
    'X-UT-Route': auth.route ?? 'utas.external.s2.fut.ea.com',
    'Origin': 'https://www.ea.com',
    'Referer': 'https://www.ea.com/',
  };
}

function buildSearchURL(config) {
  const params = new URLSearchParams();
  params.set('type', 'player');
  params.set('num', '21');
  params.set('start', '0');
  if (config.playerName) params.set('maskedDefId', config.playerName);
  if (config.position)   params.set('pos', config.position);
  if (config.quality)    params.set('lev', config.quality);
  if (config.maxBuyNow)  params.set('maxb', String(config.maxBuyNow));
  if (config.maxBid)     params.set('maxc', String(config.maxBid));
  return `${FUT_BASE}/transfermarket?${params.toString()}`;
}

async function searchMarket(config, auth) {
  const url = buildSearchURL(config);
  const resp = await fetch(url, { method: 'GET', headers: buildHeaders(auth) });

  if (resp.status === 401 || resp.status === 403) throw new Error(`AUTH_EXPIRED:${resp.status}`);
  if (resp.status === 429) throw new Error('RATE_LIMITED');
  if (!resp.ok) throw new Error(`HTTP:${resp.status}`);

  const newPT = resp.headers.get('X-UT-PHISHING-TOKEN');
  if (newPT) auth.phishingToken = newPT;

  const body = await resp.json();
  return body.auctionInfo ?? [];
}

async function buyNow(tradeId, buyNowPrice, auth) {
  const url = `${FUT_BASE}/trade/${tradeId}/bid`;
  const resp = await fetch(url, {
    method: 'PUT',
    headers: buildHeaders(auth),
    body: JSON.stringify({ bid: buyNowPrice }),
  });

  if (resp.status === 401 || resp.status === 403) throw new Error(`AUTH_EXPIRED:${resp.status}`);

  const body = await resp.json().catch(() => ({}));
  const newPT = resp.headers.get('X-UT-PHISHING-TOKEN');
  if (newPT) auth.phishingToken = newPT;

  return { ok: resp.ok, status: resp.status, body };
}

async function placeBid(tradeId, bidAmount, auth) {
  const url = `${FUT_BASE}/trade/${tradeId}/bid`;
  const resp = await fetch(url, {
    method: 'PUT',
    headers: buildHeaders(auth),
    body: JSON.stringify({ bid: bidAmount }),
  });

  const body = await resp.json().catch(() => ({}));
  const newPT = resp.headers.get('X-UT-PHISHING-TOKEN');
  if (newPT) auth.phishingToken = newPT;

  return { ok: resp.ok, status: resp.status, body };
}

function roundToBracket(amount) {
  if (amount < 1000)    return Math.ceil(amount / 50)   * 50;
  if (amount < 10000)   return Math.ceil(amount / 100)  * 100;
  if (amount < 50000)   return Math.ceil(amount / 250)  * 250;
  if (amount < 100000)  return Math.ceil(amount / 500)  * 500;
  if (amount < 1000000) return Math.ceil(amount / 1000) * 1000;
  return Math.ceil(amount / 5000) * 5000;
}

async function processResults(auctions, config, auth) {
  for (const auction of auctions) {
    const { buyNowPrice, currentBid, tradeId, expires } = auction;
    if (expires === -1) continue;

    if (
      (config.autoBuyMode === 'buynow' || config.autoBuyMode === 'both') &&
      buyNowPrice > 0 &&
      buyNowPrice <= config.maxBuyNow
    ) {
      const profit = config.marketValue ? futTax(config.marketValue) - buyNowPrice : 0;
      if (config.minProfit > 0 && profit < config.minProfit && config.marketValue) {
        log('miss', `Skip — profit ${profit} < min ${config.minProfit}`);
        continue;
      }

      log('info', `BN attempt: ${auction.itemData?.lastName ?? 'player'} @ ${buyNowPrice}c [${tradeId}]`);
      const result = await buyNow(tradeId, buyNowPrice, auth);

      if (result.ok) {
        sniperState.stats.buys++;
        sniperState.stats.coinsSaved += (config.maxBuyNow - buyNowPrice);
        updateStats();
        log('buy', `BOUGHT @ ${buyNowPrice}c — saved ${config.maxBuyNow - buyNowPrice}c — trade ${tradeId}`);
        return true;
      } else {
        log('miss', `Buy failed [${result.status}] — ${JSON.stringify(result.body).slice(0, 80)}`);
      }
    }

    if (
      (config.autoBuyMode === 'bid' || config.autoBuyMode === 'both') &&
      config.maxBid > 0 &&
      expires > 5
    ) {
      const nextBid = roundToBracket(currentBid + 50);
      if (nextBid <= config.maxBid) {
        const result = await placeBid(tradeId, nextBid, auth);
        if (result.ok) log('buy', `BID placed ${nextBid}c on ${tradeId}`);
      }
    }
  }
  return false;
}

async function sniperLoop() {
  if (!sniperState.running) return;

  const { config, auth } = sniperState;
  const jitter = Math.floor(Math.random() * 100) - 50;
  const delay  = Math.max(200, config.interval + jitter);

  try {
    sniperState.stats.searches++;
    updateStats();

    const auctions = await searchMarket(config, auth);
    if (auctions.length > 0) {
      log('info', `[${sniperState.stats.searches}] Found ${auctions.length} results`);
      await processResults(auctions, config, auth);
    }

    await chrome.storage.session.set({ futAuth: auth });

  } catch (err) {
    const msg = err.message ?? String(err);
    if (msg.startsWith('AUTH_EXPIRED')) {
      log('err', 'Session expired — open FUT Web App to refresh auth');
      sniperState.running = false;
      chrome.runtime.sendMessage({ type: 'STOPPED', reason: 'auth_expired' }).catch(() => {});
      return;
    }
    if (msg === 'RATE_LIMITED') {
      log('err', 'Rate limited — backing off 10s');
      await sleep(10000);
    } else {
      log('err', `Error: ${msg}`);
      await sleep(2000);
    }
  }

  if (sniperState.running) {
    sniperState.loopTimer = setTimeout(sniperLoop, delay);
  }
}

chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  switch (msg.type) {
    case 'AUTH_UPDATE': {
      sniperState.auth = { ...sniperState.auth, ...msg.payload };
      chrome.storage.session.set({ futAuth: sniperState.auth });
      sendResponse({ ok: true });
      break;
    }
    case 'START': {
      if (sniperState.running) { sendResponse({ ok: false, reason: 'already_running' }); break; }
      sniperState.config = msg.config;
      sniperState.stats  = { searches: 0, buys: 0, coinsSaved: 0 };
      sniperState.running = true;
      chrome.storage.session.get('futAuth', (data) => {
        if (data.futAuth) sniperState.auth = data.futAuth;
        if (!sniperState.auth?.sid) {
          log('err', 'No auth tokens — open FUT Web App and make any action first');
          sniperState.running = false;
          sendResponse({ ok: false, reason: 'no_auth' });
          return;
        }
        log('info', `Sniper started — target max ${msg.config.maxBuyNow}c — interval ${msg.config.interval}ms`);
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
        running: sniperState.running,
        hasAuth: !!sniperState.auth?.sid,
        stats: sniperState.stats,
      });
      break;
    }
  }
});

chrome.runtime.onMessage.addListener((msg) => {
  if (msg.type === 'FUT_TOKENS_RELAY') {
    const tokens = msg.payload;
    if (tokens.sid || tokens.phishingToken || tokens.nucleusId) {
      sniperState.auth = {
        sid:           tokens.sid           ?? sniperState.auth?.sid,
        phishingToken: tokens.phishingToken ?? sniperState.auth?.phishingToken,
        nucleusId:     tokens.nucleusId     ?? sniperState.auth?.nucleusId,
        route:         tokens.route         ?? sniperState.auth?.route,
      };
      chrome.storage.session.set({ futAuth: sniperState.auth });
    }
  }
});
