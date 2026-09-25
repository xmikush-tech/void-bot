"""
main.py — avvia bot Discord + FastAPI HTTP server in parallelo
Railway esegue questo come entrypoint
"""
import asyncio, os, threading, uvicorn
import db
from bot import bot
from api import app

def run_api():
    port = int(os.environ.get("PORT", 8000))
    uvicorn.run(app, host="0.0.0.0", port=port, log_level="warning")

async def main():
    db.init_db()
    # API in thread separato
    t = threading.Thread(target=run_api, daemon=True)
    t.start()
    print(f"[VOID] API avviata")
    # Bot in asyncio
    await bot.start(os.environ["DISCORD_TOKEN"])

if __name__ == "__main__":
    asyncio.run(main())
