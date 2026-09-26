"""
api.py — VOID HTTP API
Loader chiama POST /auth/password con la OTP generata da /password nel bot
"""
from fastapi import FastAPI, Header, HTTPException
from pydantic import BaseModel
import db, time, os

app = FastAPI(title="VOID API", docs_url=None, redoc_url=None)

class AuthResponse(BaseModel):
    valid:      bool
    message:    str
    product:    str = ""
    discord_id: str = ""
    expires_at: int = 0

# ── POST /auth/password — endpoint principale del loader ─────────────
class PasswordRequest(BaseModel):
    password: str   # OTP alfanumerico 12-char da /password nel bot
    hwid: str       # hardware ID della macchina

@app.post("/auth/password", response_model=AuthResponse)
async def auth_password(body: PasswordRequest):
    otp  = body.password.strip()
    hwid = body.hwid.strip()

    if not otp or not hwid:
        return AuthResponse(valid=False, message="Parametri mancanti.")

    ok, user, msg = db.validate_otp(otp, hwid)
    if not ok:
        return AuthResponse(valid=False, message=msg)

    return AuthResponse(
        valid=True,
        message="OK",
        product=user["product"],
        discord_id=user["discord_id"],
        expires_at=user["expires_at"] or 0
    )

# ── POST /auth/pin — legacy, rimane per compatibilità ────────────────
class PinRequest(BaseModel):
    pin:  str
    hwid: str

@app.post("/auth/pin", response_model=AuthResponse)
async def auth_pin(body: PinRequest):
    ok, user, msg = db.validate_otp(body.pin.strip(), body.hwid.strip())
    if not ok:
        return AuthResponse(valid=False, message=msg)
    return AuthResponse(
        valid=True, message="OK",
        product=user["product"], discord_id=user["discord_id"],
        expires_at=user["expires_at"] or 0
    )

@app.get("/health")
async def health():
    return {"status": "ok", "ts": int(time.time())}

# ── GET /key/{key} — info su una key (chiamato dall'Express backend) ──────
_INTERNAL_SECRET = os.environ.get("INTERNAL_SECRET", "")

@app.get("/key/{key}")
async def get_key_info(key: str, x_internal_secret: str = Header(default="")):
    if _INTERNAL_SECRET and x_internal_secret != _INTERNAL_SECRET:
        raise HTTPException(status_code=403, detail="Forbidden")
    with db.conn() as c:
        row = c.execute("SELECT * FROM keys WHERE key=?", (key,)).fetchone()
        if not row:
            return {"valid": False, "message": "Key non trovata."}
        user = c.execute("SELECT * FROM users WHERE key=?", (key,)).fetchone()
        return {
            "valid": True,
            "product": row["product"],
            "discord_id": user["discord_id"] if user else None,
            "expires_at": user["expires_at"] if user else 0,
            "message": "OK",
        }
