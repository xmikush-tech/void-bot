"""
bot.py — VOID Discord Bot
Comandi: /redeem /pin /status /genkey /resethwid
"""
import discord, os, time, asyncio
import urllib.request as _urllib_req
import json as _json
from discord import app_commands
from discord.ext import commands
import db

def _sync_redeem_to_backend(discord_id: str, license_key: str, product: str, expires_at):
    """Fire-and-forget: tells Express backend about a freshly redeemed key."""
    backend_url = os.environ.get("BACKEND_URL", "").rstrip("/")
    secret      = os.environ.get("INTERNAL_SECRET", "")
    if not backend_url:
        return
    try:
        body = _json.dumps({
            "discord_id":  discord_id,
            "license_key": license_key,
            "product":     product,
            "expires_at":  expires_at or 0,
        }).encode()
        req = _urllib_req.Request(
            f"{backend_url}/api/internal/sync-redeem",
            data=body,
            headers={"Content-Type": "application/json", "X-Internal-Secret": secret},
            method="POST",
        )
        _urllib_req.urlopen(req, timeout=5)
    except Exception:
        pass  # non-blocking — redemption succeeds regardless

GUILD_ID       = int(os.environ["GUILD_ID"])          # ID server Discord
CUSTOMER_ROLE  = int(os.environ["CUSTOMER_ROLE_ID"])  # ruolo da assegnare
ADMIN_ROLE     = int(os.environ["ADMIN_ROLE_ID"])      # ruolo admin
REDEEM_CHANNEL = int(os.environ.get("REDEEM_CHANNEL_ID", 0))  # opzionale

intents = discord.Intents.default()
intents.members = True

bot = commands.Bot(command_prefix="!", intents=intents)
tree = bot.tree

@bot.event
async def on_ready():
    db.init_db()
    await tree.sync(guild=discord.Object(id=GUILD_ID))
    print(f"[VOID] Bot online: {bot.user} | Guild: {GUILD_ID}")

# ── /redeem ───────────────────────────────────────────────────────────────
@tree.command(name="redeem", description="Riscatta la tua license key",
              guild=discord.Object(id=GUILD_ID))
@app_commands.describe(key="La tua key VOID-XXXX-XXXX-XXXX-XXXX")
async def redeem(interaction: discord.Interaction, key: str):
    await interaction.response.defer(ephemeral=True)

    uid  = str(interaction.user.id)
    uname = str(interaction.user)
    key  = key.strip().upper()

    ok, msg, product = db.redeem_key(key, uid, uname)

    if ok:
        user_row = db.get_user(uid)
        _sync_redeem_to_backend(uid, key, product, user_row["expires_at"] if user_row else 0)

    if not ok:
        embed = discord.Embed(
            title="❌ Errore",
            description=msg,
            color=0xe53e3e
        )
        await interaction.followup.send(embed=embed, ephemeral=True)
        return

    # assegna ruolo Customer
    guild  = bot.get_guild(GUILD_ID)
    member = guild.get_member(interaction.user.id)
    role   = guild.get_role(CUSTOMER_ROLE)
    if member and role:
        await member.add_roles(role)

    embed = discord.Embed(
        title="✅ Key Riscattata",
        description=f"Benvenuto nel VOID!\n\n**Prodotto:** {product}",
        color=0x38a169
    )
    embed.add_field(
        name="Come iniziare",
        value="1. Scarica il loader\n2. Usa `/pin` per generare il tuo PIN\n3. Inserisci il PIN nel loader e premi Play",
        inline=False
    )
    embed.set_footer(text="void.xyz")
    await interaction.followup.send(embed=embed, ephemeral=True)

# ── /password ────────────────────────────────────────────────────────────────
@tree.command(name="password", description="Genera una password temporanea per il loader Void",
              guild=discord.Object(id=GUILD_ID))
async def password(interaction: discord.Interaction):
    await interaction.response.defer(ephemeral=True)

    uid  = str(interaction.user.id)
    user = db.get_user(uid)

    if not user:
        embed = discord.Embed(
            title="❌ Accesso negato",
            description="Non hai una licenza attiva. Usa `/redeem` prima.",
            color=0xe53e3e
        )
        await interaction.followup.send(embed=embed, ephemeral=True)
        return

    if user["expires_at"] and user["expires_at"] < int(time.time()):
        embed = discord.Embed(
            title="❌ Licenza scaduta",
            description="La tua licenza è scaduta. Contatta il supporto.",
            color=0xe53e3e
        )
        await interaction.followup.send(embed=embed, ephemeral=True)
        return

    otp = db.generate_otp(uid)

    embed = discord.Embed(color=0x1a1a1a)
    embed.add_field(
        name="✅  Generated",
        value=f"Eliminazione tra **20 secondi**.\n\n**Password Generata**\n```{otp}```",
        inline=False
    )
    embed.set_footer(text=f"Prodotto: {user['product']}  •  void.xyz")

    await interaction.followup.send(embed=embed, ephemeral=True)

    # auto-elimina dopo 20s — stessa logica del bot XYZ
    await asyncio.sleep(20)
    try:
        await interaction.delete_original_response()
    except Exception:
        pass

# legacy /pin alias — ridiretta a /password così non rompe chi l'aveva salvato
@tree.command(name="pin", description="Usa /password invece",
              guild=discord.Object(id=GUILD_ID))
async def pin(interaction: discord.Interaction):
    await interaction.response.send_message(
        "Usa `/password` per ottenere la tua password temporanea.",
        ephemeral=True
    )

# ── /status ───────────────────────────────────────────────────────────────
@tree.command(name="status", description="Controlla la tua licenza",
              guild=discord.Object(id=GUILD_ID))
async def status(interaction: discord.Interaction):
    await interaction.response.defer(ephemeral=True)

    uid  = str(interaction.user.id)
    user = db.get_user(uid)

    if not user:
        await interaction.followup.send(
            embed=discord.Embed(
                title="❌ Nessuna licenza",
                description="Usa `/redeem <key>` per attivare.",
                color=0xe53e3e
            ), ephemeral=True
        )
        return

    now    = int(time.time())
    exp    = user["expires_at"]
    active = exp is None or exp > now

    if exp:
        remaining = max(0, exp - now)
        days  = remaining // 86400
        hours = (remaining % 86400) // 3600
        exp_str = f"{days}d {hours}h"
    else:
        exp_str = "Permanente"

    embed = discord.Embed(
        title="📋 Stato Licenza",
        color=0x38a169 if active else 0xe53e3e
    )
    embed.add_field(name="Prodotto",  value=user["product"],                   inline=True)
    embed.add_field(name="Stato",     value="✅ Attiva" if active else "❌ Scaduta", inline=True)
    embed.add_field(name="Scade in",  value=exp_str,                           inline=True)
    embed.add_field(name="HWID",      value="Legato" if user["hwid"] else "Non legato", inline=True)
    embed.set_footer(text="void.xyz")
    await interaction.followup.send(embed=embed, ephemeral=True)

# ── /genkey (admin only) ────────────────────────────────────────────────
@tree.command(name="genkey", description="[ADMIN] Genera una nuova license key",
              guild=discord.Object(id=GUILD_ID))
@app_commands.describe(
    product="Prodotto (default: FiveM External)",
    days="Durata in giorni (default: 30)"
)
async def genkey(interaction: discord.Interaction,
                 product: str = "FiveM External",
                 days: int = 30):
    await interaction.response.defer(ephemeral=True)

    # check ruolo admin
    admin_role = discord.utils.get(interaction.user.roles, id=ADMIN_ROLE)
    if not admin_role:
        await interaction.followup.send("❌ Non hai i permessi.", ephemeral=True)
        return

    key = db.create_key(product=product, duration_days=days)

    embed = discord.Embed(title="🔑 Key Generata", color=0x805ad5)
    embed.add_field(name="Key",      value=f"```{key}```", inline=False)
    embed.add_field(name="Prodotto", value=product,         inline=True)
    embed.add_field(name="Durata",   value=f"{days} giorni", inline=True)
    embed.set_footer(text="Invia questa key al customer • void.xyz")
    await interaction.followup.send(embed=embed, ephemeral=True)

# ── /resethwid (admin only) ───────────────────────────────────────────────
@tree.command(name="resethwid", description="[ADMIN] Resetta l'HWID di un utente",
              guild=discord.Object(id=GUILD_ID))
@app_commands.describe(user="L'utente Discord")
async def resethwid(interaction: discord.Interaction, user: discord.Member):
    await interaction.response.defer(ephemeral=True)

    admin_role = discord.utils.get(interaction.user.roles, id=ADMIN_ROLE)
    if not admin_role:
        await interaction.followup.send("❌ Non hai i permessi.", ephemeral=True)
        return

    ok = db.reset_hwid(str(user.id))
    if ok:
        embed = discord.Embed(
            title="✅ HWID Resettato",
            description=f"L'HWID di {user.mention} è stato resettato.",
            color=0x38a169
        )
    else:
        embed = discord.Embed(
            title="❌ Errore",
            description="Utente non trovato nel DB.",
            color=0xe53e3e
        )
    await interaction.followup.send(embed=embed, ephemeral=True)
