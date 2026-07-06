#!/bin/bash
# ─────────────────────────────────────────────────────────────────────
# Back up the Tornado 3D leaderboard database.
#
# Uses SQLite's online ".backup" so it is safe while the service writes.
# Keeps the newest KEEP backups and prunes the rest.
#
# Wire it up with cron, e.g. every 6 hours:
#   0 */6 * * * /home/micu/tornado/server/backup_scores.sh >> /home/micu/tornado/server/backup.log 2>&1
# ─────────────────────────────────────────────────────────────────────
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DB="$DIR/scores.db"
BACKUP_DIR="${BACKUP_DIR:-$DIR/backups}"
KEEP="${KEEP:-14}"

if [ ! -f "$DB" ]; then
    echo "[$(date -Is)] no database at $DB, nothing to back up"
    exit 0
fi

mkdir -p "$BACKUP_DIR"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="$BACKUP_DIR/scores-$STAMP.db"

# Online backup (consistent even while the API is writing)
sqlite3 "$DB" ".backup '$OUT'"
echo "[$(date -Is)] backed up -> $OUT ($(stat -c%s "$OUT") bytes)"

# Prune: keep only the newest $KEEP backups
ls -1t "$BACKUP_DIR"/scores-*.db 2>/dev/null | tail -n +"$((KEEP + 1))" | while read -r f; do
    rm -f "$f" && echo "[$(date -Is)] pruned $f"
done

exit 0
