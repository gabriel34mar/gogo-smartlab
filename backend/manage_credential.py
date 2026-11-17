"""Add or disable a test RFID credential in the local SQLite database."""

import argparse
import re
from datetime import datetime, timezone

from server import connect_db, initialize


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("action", choices=("add", "disable"))
parser.add_argument("uid", help="RFID UID in hexadecimal, e.g. 04A1B2C3")
parser.add_argument("label", nargs="?", default="Test user", help="non-sensitive display label")
args = parser.parse_args()

uid = args.uid.upper().replace(":", "").replace(" ", "")
if not re.fullmatch(r"[0-9A-F]{4,20}", uid):
    parser.error("UID must contain 4-20 hexadecimal characters")

initialize()
with connect_db() as db:
    if args.action == "add":
        db.execute("""
            INSERT INTO credentials(card_uid, user_label, active, updated_at)
            VALUES (?, ?, 1, ?)
            ON CONFLICT(card_uid) DO UPDATE SET
                user_label = excluded.user_label,
                active = 1,
                updated_at = excluded.updated_at
        """, (uid, args.label, datetime.now(timezone.utc).isoformat()))
        print(f"Enabled credential {uid} ({args.label})")
    else:
        cursor = db.execute("UPDATE credentials SET active = 0, updated_at = ? WHERE card_uid = ?",
                            (datetime.now(timezone.utc).isoformat(), uid))
        print(f"Disabled credential {uid}" if cursor.rowcount else f"Credential {uid} was not found")
