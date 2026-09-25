"""
bot.py — VOID Discord Bot
Comandi: /redeem /pin /status /genkey /resethwid
"""
import discord, os, time
from discord import app_commands
from discord.ext import commands
import db

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

# ── /redeem ──────────────────────────────────────────────────────────────────
@tree.command(name="redeem", description="Riscatta la tua license key",
              guild=discord.Object(id=GUILD_ID))
@app_commands.describe(key="La tua key VOID-XXXX-XXXX-XXXX-XXXX")
async def redeem(interaction: discord.Interaction, key: str):
    await interaction.response.defer(ephemeral=True)

    uid  = str(interaction.user.id)
    uname = str(interaction.user)
    key  = key.strip().upper()

    ok, msg, product = db.redeem_key(key, uid, uname)

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

# ── /pin ─────────────────────────────────────────────────────────────────────
@tree.command(name="pin", description="Genera un PIN temporaneo per il loader",
              guild=discord.Object(id=GUILD_ID))
async def pin(interaction: discord.Interaction):
    await interaction.response.defer(ephemeral=True)

    uid  = str(interaction.user.id)
    user = db.get_user(uid)

    if not user:
        await interaction.followup.send(
            embed=discord.Embed(
                title="❌ Accesso negato",
                description="Non hai una licenza attiva. Usa `/redeem` prima.",
                color=0xe53e3e
            ), ephemeral=True
        )
        return

    if user["expires_at"] and user["expires_at"] < int(time.time()):
        await interaction.followup.send(
            embed=discord.Embed(
                title="❌ Licenza scaduta",
                description="La tua licenza è scaduta. Contatta il supporto.",
                color=0xe53e3e
            ), ephemeral=True
        )
        return

    pin_code = db.generate_pin(uid)

    embed = discord.Embed(
        title="🔑 Authentication PIN Generated",
        color=0x2d3748
    )
    embed.add_field(name="Il tuo PIN è", value=f"```{pin_code}```", inline=False)
    embed.add_field(
        name="Come usarlo",
        value="1. Apri il VOID Loader\n2. Inserisci questo PIN\n3. Premi Sign In",
        inline=False
    )
    embed.add_field(name="⏱ Scade in", value="30 secondi", inline=True)
    embed.add_field(name="Prodotto", value=user["product"], inline=True)
    embed.set_footer(text="Non condividere mai il tuo PIN • void.xyz")

    # Prova a mandare in DM, fallback nella chat ephemeral
    try:
        await interaction.user.send(embed=embed)
        await interaction.followup.send("📬 PIN inviato in DM!", ephemeral=True)
    except discord.Forbidden:
        await interaction.followup.send(embed=embed, ephemeral=True)

# ── /status ──────────────────────────────────────────────────────────────────
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

# ── /genkey (admin only) ──────────────────────────────────────────────────────
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

# ── /resethwid (admin only) ───────────────────────────────────────────────────
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
