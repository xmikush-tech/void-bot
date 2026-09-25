"""
db.py — VOIDLoader SQLite layer
Tables: keys, users, pins
"""
import sqlite3, os, secrets, string, time
from contextlib import contextmanager

DB_PATH = os.environ.get("DB_PATH", "void.db")

@contextmanager
def conn():
    c = sqlite3.connect(DB_PATH, check_same_thread=False)
    c.row_factory = sqlite3.Row
    try:
        yield c
        c.commit()
    finally:
        c.close()

def init_db():
    with conn() as c:
        c.executescript("""
        CREATE TABLE IF NOT EXISTS keys (
            key         TEXT PRIMARY KEY,
            product     TEXT NOT NULL DEFAULT 'FiveM External',
            duration    INTEGER NOT NULL DEFAULT 30,  -- giorni
            redeemed    INTEGER NOT NULL DEFAULT 0,
            redeemed_by TEXT,       -- discord user id
            redeemed_at INTEGER,
            expires_at  INTEGER,
            created_at  INTEGER DEFAULT (strftime('%s','now'))
        );

        CREATE TABLE IF NOT EXISTS users (
            discord_id  TEXT PRIMARY KEY,
            username    TEXT,
            key         TEXT,
            product     TEXT,
            hwid        TEXT,
            expires_at  INTEGER,
            created_at  INTEGER DEFAULT (strftime('%s','now'))
        );

        CREATE TABLE IF NOT EXISTS pins (
            pin         TEXT PRIMARY KEY,
            discord_id  TEXT NOT NULL,
            expires_at  INTEGER NOT NULL,
            used        INTEGER NOT NULL DEFAULT 0
        );
        """)

# ── Key generation ──────────────────────────────────────────────────────────
def _rand_seg(n=4):
    chars = string.ascii_uppercase + string.digits
    return ''.join(secrets.choice(chars) for _ in range(n))

def generate_key():
    return f"VOID-{_rand_seg()}-{_rand_seg()}-{_rand_seg()}-{_rand_seg()}"

def create_key(product="FiveM External", duration_days=30):
    key = generate_key()
    with conn() as c:
        c.execute(
            "INSERT INTO keys (key, product, duration) VALUES (?,?,?)",
            (key, product, duration_days)
        )
    return key

def redeem_key(key: str, discord_id: str, username: str, hwid: str = None):
    """Returns (ok, message, product)"""
    with conn() as c:
        row = c.execute("SELECT * FROM keys WHERE key=?", (key,)).fetchone()
        if not row:
            return False, "Key non valida.", None
        if row["redeemed"]:
            return False, "Key già riscattata.", None

        now = int(time.time())
        expires = now + row["duration"] * 86400

        c.execute("""
            UPDATE keys SET redeemed=1, redeemed_by=?, redeemed_at=?, expires_at=?
            WHERE key=?
        """, (discord_id, now, expires, key))

        # upsert user
        c.execute("""
            INSERT INTO users (discord_id, username, key, product, hwid, expires_at)
            VALUES (?,?,?,?,?,?)
            ON CONFLICT(discord_id) DO UPDATE SET
                key=excluded.key,
                product=excluded.product,
                hwid=excluded.hwid,
                expires_at=excluded.expires_at,
                username=excluded.username
        """, (discord_id, username, key, row["product"], hwid, expires))

        return True, f"Key riscattata! Prodotto: **{row['product']}**", row["product"]

def get_user(discord_id: str):
    with conn() as c:
        return c.execute("SELECT * FROM users WHERE discord_id=?", (discord_id,)).fetchone()

def reset_hwid(discord_id: str):
    with conn() as c:
        c.execute("UPDATE users SET hwid=NULL WHERE discord_id=?", (discord_id,))
        return c.rowcount > 0

# ── PIN system ───────────────────────────────────────────────────────────────
PIN_TTL = 30  # secondi

def generate_pin(discord_id: str) -> str:
    pin = ''.join(secrets.choice(string.digits) for _ in range(6))
    expires = int(time.time()) + PIN_TTL
    with conn() as c:
        # invalida PIN vecchi dello stesso user
        c.execute("DELETE FROM pins WHERE discord_id=?", (discord_id,))
        c.execute(
            "INSERT INTO pins (pin, discord_id, expires_at) VALUES (?,?,?)",
            (pin, discord_id, expires)
        )
    return pin

def validate_pin(pin: str, hwid: str):
    """Returns (ok, user_row_or_None, message)"""
    now = int(time.time())
    with conn() as c:
        row = c.execute(
            "SELECT * FROM pins WHERE pin=? AND used=0 AND expires_at>?",
            (pin, now)
        ).fetchone()
        if not row:
            return False, None, "PIN non valido o scaduto."

        user = c.execute(
            "SELECT * FROM users WHERE discord_id=?",
            (row["discord_id"],)
        ).fetchone()
        if not user:
            return False, None, "Utente non trovato."
        if user["expires_at"] and user["expires_at"] < now:
            return False, None, "Licenza scaduta."

        # lega HWID se non c'è, altrimenti verifica
        if user["hwid"] is None:
            c.execute("UPDATE users SET hwid=? WHERE discord_id=?",
                      (hwid, row["discord_id"]))
        elif user["hwid"] != hwid:
            return False, None, "HWID mismatch. Contatta il supporto."

        # marca PIN usato
        c.execute("UPDATE pins SET used=1 WHERE pin=?", (pin,))

        return True, dict(user), "OK"
