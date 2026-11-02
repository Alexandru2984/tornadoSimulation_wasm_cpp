#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────
# Back up the Tornado 3D leaderboard database (safe while the API writes).
#
#   1. SQLite online .backup (consistent snapshot, WAL included)
#   2. integrity check of the snapshot
#   3. gzip, then decompress the archive again and re-check it
#   4. atomic rename into place + .sha256 next to it
#   5. delete archives older than KEEP_DAYS
#
# Production: installed as /usr/local/sbin/tornado-leaderboard-backup and run
# daily by tornado-leaderboard-backup.timer (units in this directory).
# Restore:  gzip -cd scores-<stamp>.sqlite.gz > scores.db   (service stopped)
#
# Environment: TORNADO_DB, BACKUP_DIR, KEEP_DAYS (defaults below).
# ─────────────────────────────────────────────────────────────────────
set -Eeuo pipefail
umask 077

DB="${TORNADO_DB:-/var/lib/tornado-leaderboard/scores.db}"
BACKUP_DIR="${BACKUP_DIR:-/srv/backups/tornado-leaderboard}"
KEEP_DAYS="${KEEP_DAYS:-14}"

case "$DB$BACKUP_DIR" in *"'"*) echo "paths must not contain quotes" >&2; exit 1 ;; esac
[ -f "$DB" ] || { echo "no database at $DB" >&2; exit 1; }
[ -d "$BACKUP_DIR" ] || { echo "backup directory $BACKUP_DIR does not exist" >&2; exit 1; }
[[ "$KEEP_DAYS" =~ ^[0-9]+$ ]] || { echo "KEEP_DAYS must be a number" >&2; exit 1; }

# WAL mode: SQLite creates scores.db-wal/-shm for whoever opens the database
# while they are missing. If that were this script (root), the service user
# could no longer open its own database. The service keeps them open while
# it runs, so wait for them briefly (e.g. during a restart) and otherwise
# refuse rather than create them under the wrong owner.
if [ "$(od -An -tu1 -j18 -N1 "$DB" | tr -d ' ')" = 2 ] && [ "$(stat -c %u "$DB")" != "$(id -u)" ]; then
    for _ in $(seq 1 30); do
        [ -e "$DB-shm" ] && [ -e "$DB-wal" ] && break
        sleep 1
    done
    if [ ! -e "$DB-shm" ] || [ ! -e "$DB-wal" ]; then
        echo "$DB is in WAL mode but its -wal/-shm files are missing (service not running?);" \
             "not creating them as $(id -un)" >&2
        exit 1
    fi
fi

stamp="$(date -u +'%Y-%m-%dT%H%M%SZ')"
db_tmp="${BACKUP_DIR}/.scores-${stamp}.sqlite.tmp"
gz_tmp="${BACKUP_DIR}/.scores-${stamp}.sqlite.gz.tmp"
verify="${BACKUP_DIR}/.scores-${stamp}.verify.sqlite"
final="${BACKUP_DIR}/scores-${stamp}.sqlite.gz"

cleanup() { rm -f "$db_tmp" "$gz_tmp" "$verify"; }
trap cleanup EXIT

sqlite3 -readonly "$DB" ".timeout 10000" ".backup '${db_tmp}'"
[ "$(sqlite3 "$db_tmp" 'pragma quick_check;')" = ok ] || { echo "snapshot failed integrity check" >&2; exit 1; }
gzip -n -9 -c "$db_tmp" > "$gz_tmp"
gzip -t "$gz_tmp"
gzip -cd "$gz_tmp" > "$verify"
[ "$(sqlite3 "$verify" 'pragma quick_check;')" = ok ] || { echo "archive failed integrity check" >&2; exit 1; }
mv "$gz_tmp" "$final"
( cd "$BACKUP_DIR" && sha256sum "$(basename "$final")" > "$(basename "$final").sha256" )
echo "backed up $DB -> $final ($(stat -c%s "$final") bytes)"

find "$BACKUP_DIR" -xdev -maxdepth 1 -type f \
    \( -name 'scores-*.sqlite.gz' -o -name 'scores-*.sqlite.gz.sha256' \) \
    -mtime +"$KEEP_DAYS" -print -delete
