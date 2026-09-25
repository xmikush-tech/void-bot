"""
api.py — VOID HTTP API
Il loader chiama POST /auth/pin per autenticarsi
"""
from fastapi import FastAPI, Request, HTTPException
from pydantic import BaseModel
import db, time

app = FastAPI(title="VOID API", docs_url=None, redoc_url=None)

class PinRequest(BaseModel):
    pin:  str
    hwid: str

class PinResponse(BaseModel):
    valid:      bool
    message:    str
    product:    str  = ""
    discord_id: str  = ""
    expires_at: int  = 0

@app.post("/auth/pin", response_model=PinResponse)
async def auth_pin(body: PinRequest):
    pin  = body.pin.strip()
    hwid = body.hwid.strip()

    if len(pin) != 6 or not pin.isdigit():
        return PinResponse(valid=False, message="PIN malformato.")
    if not hwid:
        return PinResponse(valid=False, message="HWID mancante.")

    ok, user, msg = db.validate_pin(pin, hwid)

    if not ok:
        return PinResponse(valid=False, message=msg)

    return PinResponse(
        valid=True,
        message="OK",
        product=user["product"],
        discord_id=user["discord_id"],
        expires_at=user["expires_at"] or 0
    )

@app.get("/health")
async def health():
    return {"status": "ok", "ts": int(time.time())}
