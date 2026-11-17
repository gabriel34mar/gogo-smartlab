"""Small offline-first sync API for the GOGO SmartLab prototype.

Run on the Raspberry Pi with SMARTLAB_DEVICE_TOKEN set to a private token.
This prototype API does not make authorization decisions: the ESP32 does.
"""

from __future__ import annotations

import hashlib
import hmac
import json
import os
import re
import sqlite3
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse


ROOT = Path(__file__).resolve().parent
DB_PATH = Path(os.environ.get("SMARTLAB_DB", ROOT / "smartlab.db"))
HOST = os.environ.get("SMARTLAB_HOST", "0.0.0.0")
PORT = int(os.environ.get("SMARTLAB_PORT", "8080"))
DEVICE_TOKEN = os.environ.get("SMARTLAB_DEVICE_TOKEN", "")
UID_RE = re.compile(r"^[0-9A-Fa-f]{4,20}$")


def connect_db() -> sqlite3.Connection:
    conn = sqlite3.connect(DB_PATH, timeout=10)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA foreign_keys=ON")
    return conn


def initialize() -> None:
    DB_PATH.parent.mkdir(parents=True, exist_ok=True)
    with connect_db() as db:
        db.executescript("""
        CREATE TABLE IF NOT EXISTS credentials (
            card_uid TEXT PRIMARY KEY,
            user_label TEXT NOT NULL,
            active INTEGER NOT NULL DEFAULT 1 CHECK (active IN (0, 1)),
            updated_at TEXT NOT NULL
        );
        CREATE TABLE IF NOT EXISTS access_events (
            event_id TEXT PRIMARY KEY,
            device_id TEXT NOT NULL,
            card_uid TEXT NOT NULL,
            authorized INTEGER NOT NULL CHECK (authorized IN (0, 1)),
            occurred_at TEXT NOT NULL,
            received_at TEXT NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_events_occurred_at
            ON access_events(occurred_at DESC);
        """)


class Handler(BaseHTTPRequestHandler):
    server_version = "SmartLabSync/0.1"

    def send_json(self, status: int, payload: dict) -> None:
        body = json.dumps(payload, separators=(",", ":")).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def authorized_device(self) -> bool:
        supplied = self.headers.get("Authorization", "")
        expected = f"Bearer {DEVICE_TOKEN}"
        return bool(DEVICE_TOKEN) and hmac.compare_digest(supplied, expected)

    def read_json(self) -> dict:
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError as exc:
            raise ValueError("invalid Content-Length") from exc
        if length < 1 or length > 64_000:
            raise ValueError("body must be between 1 and 64000 bytes")
        try:
            data = json.loads(self.rfile.read(length))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ValueError("body must be valid JSON") from exc
        if not isinstance(data, dict):
            raise ValueError("JSON body must be an object")
        return data

    def do_GET(self) -> None:  # noqa: N802
        path = urlparse(self.path).path
        if path == "/health":
            self.send_json(200, {"status": "ok"})
            return
        if not self.authorized_device():
            self.send_json(401, {"error": "unauthorized"})
            return
        if path == "/v1/credentials":
            with connect_db() as db:
                rows = db.execute(
                    "SELECT card_uid FROM credentials WHERE active = 1 ORDER BY card_uid"
                ).fetchall()
            uids = [row["card_uid"] for row in rows]
            version = hashlib.sha256("\n".join(uids).encode()).hexdigest()[:16]
            self.send_json(200, {"version": version, "credentials": uids})
            return
        self.send_json(404, {"error": "not_found"})

    def do_POST(self) -> None:  # noqa: N802
        if not self.authorized_device():
            self.send_json(401, {"error": "unauthorized"})
            return
        if urlparse(self.path).path != "/v1/events/batch":
            self.send_json(404, {"error": "not_found"})
            return
        try:
            body = self.read_json()
            events = body.get("events")
            if not isinstance(events, list) or len(events) > 100:
                raise ValueError("events must be a list of at most 100 records")
            normalized = []
            for event in events:
                if not isinstance(event, dict):
                    raise ValueError("each event must be an object")
                event_id = event.get("event_id")
                device_id = event.get("device_id")
                uid = event.get("card_uid")
                decision = event.get("authorized")
                occurred_at = event.get("occurred_at")
                if not all(isinstance(v, str) and 1 <= len(v) <= 80
                           for v in (event_id, device_id, occurred_at)):
                    raise ValueError("event_id, device_id, and occurred_at are required")
                if not isinstance(uid, str) or not UID_RE.fullmatch(uid):
                    raise ValueError("card_uid must be 4-20 hexadecimal characters")
                if type(decision) is not bool:
                    raise ValueError("authorized must be a boolean")
                try:
                    datetime.fromisoformat(occurred_at.replace("Z", "+00:00"))
                except ValueError as exc:
                    raise ValueError("occurred_at must be an ISO-8601 timestamp") from exc
                normalized.append((event_id, device_id, uid.upper(), int(decision), occurred_at))
        except ValueError as exc:
            self.send_json(400, {"error": "invalid_request", "detail": str(exc)})
            return

        inserted = 0
        now = datetime.now(timezone.utc).isoformat()
        try:
            with connect_db() as db:
                for row in normalized:
                    cursor = db.execute("""
                        INSERT OR IGNORE INTO access_events
                            (event_id, device_id, card_uid, authorized, occurred_at, received_at)
                        VALUES (?, ?, ?, ?, ?, ?)
                    """, (*row, now))
                    inserted += cursor.rowcount
        except sqlite3.Error:
            self.send_json(500, {"error": "storage_error"})
            return
        self.send_json(200, {"accepted": len(normalized), "inserted": inserted})

    def log_message(self, fmt: str, *args) -> None:
        print(f"{self.address_string()} - {fmt % args}")


if __name__ == "__main__":
    if not DEVICE_TOKEN:
        raise SystemExit("Set SMARTLAB_DEVICE_TOKEN before starting the API.")
    initialize()
    print(f"SmartLab sync API listening on {HOST}:{PORT}; database: {DB_PATH}")
    ThreadingHTTPServer((HOST, PORT), Handler).serve_forever()
