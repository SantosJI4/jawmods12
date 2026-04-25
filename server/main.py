import os
import sqlite3
import secrets
import string
import hashlib
import time
from datetime import datetime, timedelta
from flask import Flask, request, jsonify, send_from_directory

app = Flask(__name__)

DB_PATH   = os.path.join(os.path.dirname(__file__), "keys.db")
ADMIN_TOKEN = os.environ.get("ADMIN_TOKEN", "jawmods-admin-2026")
PANEL_DIR   = os.path.dirname(__file__)

# ══════════════════════════════════════
# Database
# ══════════════════════════════════════

def get_db():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn

def init_db():
    conn = get_db()
    conn.execute("""
        CREATE TABLE IF NOT EXISTS keys (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            key_value TEXT UNIQUE NOT NULL,
            hwid TEXT DEFAULT NULL,
            duration_hours INTEGER NOT NULL,
            created_at TEXT NOT NULL,
            expires_at TEXT NOT NULL,
            used INTEGER DEFAULT 0,
            paused INTEGER DEFAULT 0,
            note TEXT DEFAULT ''
        )
    """)
    # Migração: adicionar coluna paused se não existir (banco antigo)
    try:
        conn.execute("ALTER TABLE keys ADD COLUMN paused INTEGER DEFAULT 0")
        conn.commit()
    except Exception:
        pass
    conn.commit()
    conn.close()

init_db()

# ══════════════════════════════════════
# Key generation
# ══════════════════════════════════════

def generate_key():
    chars = string.ascii_uppercase + string.digits
    block = lambda: ''.join(secrets.choice(chars) for _ in range(4))
    return "JAWM-{}-{}-{}".format(block(), block(), block())

def get_hwid(android_id, device_model):
    raw = "{}:{}".format(android_id or "", device_model or "")
    return hashlib.sha256(raw.encode()).hexdigest()[:32]

# ══════════════════════════════════════
# Routes
# ══════════════════════════════════════

@app.route("/", methods=["GET"])
def index():
    return jsonify({"status": "ok", "service": "jawmods-key-server", "version": "1.0"})

@app.route("/status", methods=["GET"])
def status():
    return jsonify({"online": True, "version": "v24"})

@app.route("/validate", methods=["POST"])
def validate_key():
    data = request.get_json(silent=True)
    if not data or "key" not in data:
        return jsonify({"valid": False, "error": "Missing key"}), 400

    key_value = data["key"].strip().upper()
    android_id = data.get("android_id", "")
    device_model = data.get("device_model", "")
    hwid = get_hwid(android_id, device_model)

    conn = get_db()
    row = conn.execute("SELECT * FROM keys WHERE key_value = ?", (key_value,)).fetchone()

    if not row:
        conn.close()
        return jsonify({"valid": False, "error": "Key not found"})

    # Check paused
    if row["paused"]:
        conn.close()
        return jsonify({"valid": False, "error": "Key is paused"})

    # Check expiration
    expires_at = datetime.fromisoformat(row["expires_at"])
    if datetime.utcnow() > expires_at:
        conn.close()
        return jsonify({"valid": False, "error": "Key expired"})

    # HWID binding: first use binds the key to this device
    if row["hwid"] is None or row["hwid"] == "":
        conn.execute("UPDATE keys SET hwid = ?, used = 1 WHERE key_value = ?", (hwid, key_value))
        conn.commit()
    elif row["hwid"] != hwid:
        conn.close()
        return jsonify({"valid": False, "error": "Key bound to another device"})

    remaining_seconds = int((expires_at - datetime.utcnow()).total_seconds())
    conn.close()

    return jsonify({
        "valid": True,
        "expires_at": row["expires_at"],
        "remaining_seconds": max(0, remaining_seconds),
        "note": row["note"] or ""
    })

@app.route("/admin/generate", methods=["POST"])
def admin_generate():
    auth = request.headers.get("Authorization", "")
    if auth != "Bearer " + ADMIN_TOKEN:
        return jsonify({"error": "Unauthorized"}), 401

    data = request.get_json(silent=True) or {}
    count = min(int(data.get("count", 1)), 50)
    duration_hours = int(data.get("duration_hours", 720))  # default 30 days
    note = data.get("note", "")

    conn = get_db()
    keys = []
    now = datetime.utcnow()
    expires = now + timedelta(hours=duration_hours)

    for _ in range(count):
        key_value = generate_key()
        conn.execute(
            "INSERT INTO keys (key_value, duration_hours, created_at, expires_at, note) VALUES (?, ?, ?, ?, ?)",
            (key_value, duration_hours, now.isoformat(), expires.isoformat(), note)
        )
        keys.append(key_value)

    conn.commit()
    conn.close()

    return jsonify({
        "keys": keys,
        "duration_hours": duration_hours,
        "expires_at": expires.isoformat(),
        "note": note
    })

@app.route("/admin/list", methods=["GET"])
def admin_list():
    auth = request.headers.get("Authorization", "")
    if auth != "Bearer " + ADMIN_TOKEN:
        return jsonify({"error": "Unauthorized"}), 401

    conn = get_db()
    rows = conn.execute("SELECT key_value, hwid, duration_hours, created_at, expires_at, used, paused, note FROM keys ORDER BY id DESC LIMIT 500").fetchall()
    conn.close()

    now = datetime.utcnow()
    result = []
    for r in rows:
        d = dict(r)
        expires = datetime.fromisoformat(d["expires_at"])
        if d["paused"]:
            d["status"] = "paused"
        elif now > expires:
            d["status"] = "expired"
        else:
            d["status"] = "active"
        d["remaining_seconds"] = max(0, int((expires - now).total_seconds())) if d["status"] == "active" else 0
        result.append(d)

    return jsonify({"keys": result})


@app.route("/admin/delete", methods=["POST"])
def admin_delete():
    auth = request.headers.get("Authorization", "")
    if auth != "Bearer " + ADMIN_TOKEN:
        return jsonify({"error": "Unauthorized"}), 401

    data = request.get_json(silent=True) or {}
    key_value = data.get("key", "").strip().upper()
    if not key_value:
        return jsonify({"error": "Missing key"}), 400

    conn = get_db()
    conn.execute("DELETE FROM keys WHERE key_value = ?", (key_value,))
    conn.commit()
    conn.close()
    return jsonify({"deleted": True, "key": key_value})


@app.route("/admin/pause", methods=["POST"])
def admin_pause():
    auth = request.headers.get("Authorization", "")
    if auth != "Bearer " + ADMIN_TOKEN:
        return jsonify({"error": "Unauthorized"}), 401

    data = request.get_json(silent=True) or {}
    key_value = data.get("key", "").strip().upper()
    if not key_value:
        return jsonify({"error": "Missing key"}), 400

    conn = get_db()
    conn.execute("UPDATE keys SET paused = 1 WHERE key_value = ?", (key_value,))
    conn.commit()
    conn.close()
    return jsonify({"paused": True, "key": key_value})


@app.route("/admin/resume", methods=["POST"])
def admin_resume():
    auth = request.headers.get("Authorization", "")
    if auth != "Bearer " + ADMIN_TOKEN:
        return jsonify({"error": "Unauthorized"}), 401

    data = request.get_json(silent=True) or {}
    key_value = data.get("key", "").strip().upper()
    if not key_value:
        return jsonify({"error": "Missing key"}), 400

    conn = get_db()
    conn.execute("UPDATE keys SET paused = 0 WHERE key_value = ?", (key_value,))
    conn.commit()
    conn.close()
    return jsonify({"resumed": True, "key": key_value})


@app.route("/admin/extend", methods=["POST"])
def admin_extend():
    """Adiciona ou define horas de duração a uma key existente."""
    auth = request.headers.get("Authorization", "")
    if auth != "Bearer " + ADMIN_TOKEN:
        return jsonify({"error": "Unauthorized"}), 401

    data = request.get_json(silent=True) or {}
    key_value = data.get("key", "").strip().upper()
    hours     = int(data.get("hours", 0))
    if not key_value or hours == 0:
        return jsonify({"error": "Missing key or hours"}), 400

    conn = get_db()
    row = conn.execute("SELECT expires_at FROM keys WHERE key_value = ?", (key_value,)).fetchone()
    if not row:
        conn.close()
        return jsonify({"error": "Key not found"}), 404

    now      = datetime.utcnow()
    current  = datetime.fromisoformat(row["expires_at"])
    # Se a key já expirou, extende a partir de agora
    base     = current if current > now else now
    new_exp  = base + timedelta(hours=hours)

    conn.execute("UPDATE keys SET expires_at = ?, paused = 0 WHERE key_value = ?",
                 (new_exp.isoformat(), key_value))
    conn.commit()
    conn.close()
    return jsonify({"extended": True, "key": key_value, "new_expires_at": new_exp.isoformat()})


@app.route("/admin/reset_hwid", methods=["POST"])
def admin_reset_hwid():
    auth = request.headers.get("Authorization", "")
    if auth != "Bearer " + ADMIN_TOKEN:
        return jsonify({"error": "Unauthorized"}), 401

    data = request.get_json(silent=True) or {}
    key_value = data.get("key", "").strip().upper()
    if not key_value:
        return jsonify({"error": "Missing key"}), 400

    conn = get_db()
    conn.execute("UPDATE keys SET hwid = NULL, used = 0 WHERE key_value = ?", (key_value,))
    conn.commit()
    conn.close()
    return jsonify({"hwid_reset": True, "key": key_value})


# ── Painel Web ──────────────────────────────────────────────────────────────
@app.route("/admin/panel")
def admin_panel():
    return send_from_directory(PANEL_DIR, "panel.html")

@app.route("/admin/revoke", methods=["POST"])
def admin_revoke():
    auth = request.headers.get("Authorization", "")
    if auth != "Bearer " + ADMIN_TOKEN:
        return jsonify({"error": "Unauthorized"}), 401

    data = request.get_json(silent=True) or {}
    key_value = data.get("key", "").strip().upper()
    if not key_value:
        return jsonify({"error": "Missing key"}), 400

    conn = get_db()
    conn.execute("UPDATE keys SET expires_at = ? WHERE key_value = ?",
                 (datetime.utcnow().isoformat(), key_value))
    conn.commit()
    conn.close()

    return jsonify({"revoked": True, "key": key_value})

# ══════════════════════════════════════
# Run
# ══════════════════════════════════════

if __name__ == "__main__":
    port = int(os.environ.get("PORT", 80))
    debug = os.environ.get("DEBUG", "false").lower() == "true"
    app.run(host="0.0.0.0", port=port, debug=debug)
