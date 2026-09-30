# FC27 FUT Sniper — Chrome Extension

Transfer market sniper for EA Sports FC 27 Web App.

## Install

1. Open Chrome → `chrome://extensions`
2. Enable **Developer mode** (top right)
3. **Load unpacked** → select the `fc27-sniper/` folder
4. Pin the extension

## Usage

1. Open **FUT Web App** (`ea.com/.../web-app/`)
2. Navigate to **Transfer Market** and do **any search** (this captures your auth tokens)
3. Click the extension icon
4. Set player name, max Buy Now price, interval
5. Hit **START SNIPER**

The `AUTH OK` badge in the top-right confirms tokens were captured.

## How it works

| Layer | Job |
|---|---|
| `content.js` (MAIN world) | Hooks `XMLHttpRequest` + `fetch` before EA's JS loads; steals `X-UT-SID`, `X-UT-PHISHING-TOKEN`, `Easw-Session-Data-Nucleus-Id` from outgoing request headers |
| `injector.js` (ISOLATED world) | Bridges `window.postMessage` → `chrome.runtime.sendMessage` (cross-world relay) |
| `background.js` (service worker) | Owns the snipe loop: search → parse results → buy now / bid → jitter delay → repeat |
| `popup.html/js` | UI: config, live stats, log stream |

## Config

| Field | Effect |
|---|---|
| Player Name | Partial match against FUT's `maskedDefId` query |
| Max Buy Now | Won't buy above this. Lower = fewer hits but always profit |
| Max Bid | Max to bid on auctions (uses EA coin bracket rounding) |
| Min Profit | Skip if `futTax(marketValue) - buyNow < minProfit` |
| Interval | 300ms = turbo (rate limit risk), 750ms = default safe |
| Auto-Buy Mode | Buy Now only / Bid only / both |

## Notes

- EA's rate limit kicks in around 150-200 req/min — stay at 750ms+ to be safe
- Phishing token refreshes on every response; the extension tracks this automatically
- If auth expires mid-session, the sniper stops and you re-navigate to the transfer market
