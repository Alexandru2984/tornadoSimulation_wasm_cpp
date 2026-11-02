#!/bin/bash
# ─────────────────────────────────────────────────────────────────────
# Back up the Tornado 3D leaderboard database.
#
# Uses SQLite's online ".backup" so it is safe while the service writes.
# Keeps the newest KEEP backups and prunes the rest.
#
# Wire it up with cron or a systemd timer, e.g. every 6 hours:
#   0 */6 * * * /path/to/server/backup_scores.sh >> /var/log/tornado-backup.log 2>&1
# Must run as a user that can read the DB (root or the tornadolb user).
# ─────────────────────────────────────────────────────────────────────
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Production DB lives in /var/lib (owned by the tornadolb service user, group-
# readable). Falls back to the dev copy next to this script.
DB="${TORNADO_DB:-/var/lib/tornado-leaderboard/scores.db}"
[ -f "$DB" ] || DB="$DIR/scores.db"
BACKUP_DIR="${BACKUP_DIR:-$DIR/backups}"
KEEP="${KEEP:-14}"

if [ ! -f "$DB" ]; then
    echo "[$(date -Is)] no database at $DB, nothing to back up"
    exit 0
fi

umask 077
mkdir -p "$BACKUP_DIR"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="$BACKUP_DIR/scores-$STAMP.db"

case "$OUT" in *"'"*) echo "backup path must not contain quotes" >&2; exit 1 ;; esac

# Online backup (consistent even while the API is writing, WAL included),
# then verify the copy before trusting it.
sqlite3 -readonly "$DB" ".timeout 10000" ".backup '$OUT'"
if [ "$(sqlite3 "$OUT" 'PRAGMA quick_check;')" != "ok" ]; then
    echo "[$(date -Is)] integrity check FAILED for $OUT" >&2
    rm -f "$OUT"
    exit 1
fi
chmod 600 "$OUT"
echo "[$(date -Is)] backed up -> $OUT ($(stat -c%s "$OUT") bytes)"

# Prune: keep only the newest $KEEP backups
find "$BACKUP_DIR" -maxdepth 1 -name 'scores-*.db' -printf '%T@ %p\n' \
    | sort -rn | tail -n +"$((KEEP + 1))" | cut -d' ' -f2- \
    | while IFS= read -r f; do
        rm -f -- "$f" && echo "[$(date -Is)] pruned $f"
    done

exit 0
