// *popup.js — popup UI logic, stat polling, log rendering*
'use strict';

const $ = id => document.getElementById(id);

const statusDot    = $('statusDot');
const authBadge    = $('authBadge');
const btnStart     = $('btnStart');
const btnStop      = $('btnStop');
const logBox       = $('logBox');
const speedPreview = $('speedPreview');
const statSearches = $('statSearches');
const statBuys     = $('statBuys');
const statProfit   = $('statProfit');

function addLog(type, msg) {
  const ts  = new Date().toLocaleTimeString('en', { hour12: false });
  const div = document.createElement('div');
  div.className = `log-entry ${type}`;
  div.textContent = `[${ts}] ${msg}`;
  logBox.appendChild(div);
  while (logBox.children.length > 80) logBox.removeChild(logBox.firstChild);
  logBox.scrollTop = logBox.scrollHeight;
}

function setRunning(running) {
  if (running) {
    statusDot.classList.add('active');
    btnStart.style.display = 'none';
    btnStop.style.display  = 'block';
  } else {
    statusDot.classList.remove('active');
    btnStart.style.display = 'block';
    btnStop.style.display  = 'none';
  }
}

function fmtCoins(n) {
  if (n >= 1000000) return (n / 1000000).toFixed(2) + 'M';
  if (n >= 1000)    return (n / 1000).toFixed(1)    + 'K';
  return String(n);
}

function updateSpeedPreview() {
  const ms  = parseInt($('interval').value, 10);
  const spm = Math.round(60000 / ms);
  speedPreview.textContent = `~${spm} searches/min`;
}

function readConfig() {
  return {
    playerName:  $('playerName').value.trim(),
    position:    $('position').value,
    quality:     $('quality').value,
    maxBuyNow:   parseInt($('maxBuyNow').value, 10) || 0,
    maxBid:      parseInt($('maxBid').value, 10)    || 0,
    minProfit:   parseInt($('minProfit').value, 10) || 0,
    interval:    parseInt($('interval').value, 10)  || 750,
    autoBuyMode: $('autoBuyMode').value,
  };
}

function saveConfig(cfg) { chrome.storage.local.set({ sniperConfig: cfg }); }

function loadConfig() {
  chrome.storage.local.get('sniperConfig', (data) => {
    const cfg = data.sniperConfig;
    if (!cfg) return;
    if (cfg.playerName)  $('playerName').value  = cfg.playerName;
    if (cfg.position)    $('position').value     = cfg.position;
    if (cfg.quality)     $('quality').value      = cfg.quality;
    if (cfg.maxBuyNow)   $('maxBuyNow').value    = cfg.maxBuyNow;
    if (cfg.maxBid)      $('maxBid').value       = cfg.maxBid;
    if (cfg.minProfit)   $('minProfit').value     = cfg.minProfit;
    if (cfg.interval)    $('interval').value      = cfg.interval;
    if (cfg.autoBuyMode) $('autoBuyMode').value   = cfg.autoBuyMode;
    updateSpeedPreview();
  });
}

function checkAuth() {
  chrome.storage.session.get('futAuth', (data) => {
    const auth = data.futAuth;
    const hasAuth = auth?.sid && auth?.phishingToken;
    authBadge.textContent = hasAuth ? 'AUTH OK' : 'NO AUTH';
    authBadge.style.color = hasAuth ? '#00ff88' : '#ff6666';
  });
}

btnStart.addEventListener('click', () => {
  const cfg = readConfig();
  if (!cfg.maxBuyNow && !cfg.maxBid) {
    addLog('err', 'Set at least a Max Buy Now or Max Bid price');
    return;
  }
  saveConfig(cfg);
  addLog('info', `Starting sniper — max BN: ${cfg.maxBuyNow}c, interval: ${cfg.interval}ms`);
  chrome.runtime.sendMessage({ type: 'START', config: cfg }, (resp) => {
    if (chrome.runtime.lastError) { addLog('err', 'Background not responding — reload extension'); return; }
    if (resp?.ok) {
      setRunning(true);
    } else {
      addLog('err', `Start failed: ${resp?.reason ?? 'unknown'}`);
      if (resp?.reason === 'no_auth') addLog('info', 'Open FUT Web App, navigate to Transfer Market, then retry');
    }
  });
});

btnStop.addEventListener('click', () => {
  chrome.runtime.sendMessage({ type: 'STOP' }, () => {
    setRunning(false);
    addLog('info', 'Sniper stopped');
  });
});

$('interval').addEventListener('change', updateSpeedPreview);

chrome.runtime.onMessage.addListener((msg) => {
  if (msg.type === 'LOG')     addLog(msg.payload.type, msg.payload.msg);
  if (msg.type === 'STATS') {
    statSearches.textContent = msg.payload.searches;
    statBuys.textContent     = msg.payload.buys;
    statProfit.textContent   = fmtCoins(msg.payload.coinsSaved);
  }
  if (msg.type === 'STOPPED') {
    setRunning(false);
    addLog('err', `Sniper stopped: ${msg.reason}`);
  }
});

document.addEventListener('DOMContentLoaded', () => {
  loadConfig();
  updateSpeedPreview();
  checkAuth();
  chrome.runtime.sendMessage({ type: 'GET_STATUS' }, (resp) => {
    if (!resp) return;
    setRunning(resp.running);
    if (resp.hasAuth) { authBadge.textContent = 'AUTH OK'; authBadge.style.color = '#00ff88'; }
    if (resp.stats) {
      statSearches.textContent = resp.stats.searches;
      statBuys.textContent     = resp.stats.buys;
      statProfit.textContent   = fmtCoins(resp.stats.coinsSaved);
    }
  });
  setInterval(checkAuth, 3000);
});
