#!/usr/bin/env python3
"""Moderation tool for the Tornado 3D leaderboard.

Run it as the user that owns the database (in production: tornadolb), so
SQLite never creates files the service cannot open:

    sudo -u tornadolb python3 -B /opt/tornado-leaderboard/lbadmin.py top
    tornado-lbadmin top                 # same, via /usr/local/sbin wrapper

Commands
    top     [-m classic|timeattack] [-d 0|1|2] [-n 20]   best scores of a board
    recent  [-n 20]                                     latest submissions
    show    ID                                          one score in full
    delete  ID [ID ...]                                 remove scores
    ban     --name NAME | --score ID  [--reason ..] [--keep-scores]
            bans the name and the device (player key) behind it; their
            scores are deleted unless --keep-scores
    unban   --name NAME | --player HASH
    bans                                                list bans
    release NAME                                        free a claimed name
                                                        (owner lost the profile code)

Destructive commands ask for confirmation; -y/--yes skips it. Changes show
up on the site within a minute (the API caches global stats for 60 s).
"""
import argparse
import os
import sqlite3
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))
import leaderboard as lb  # noqa: E402

PROD_DB = "/var/lib/tornado-leaderboard/scores.db"
DIFFS = {0: "usor", 1: "normal", 2: "greu"}


def default_db():
    if os.environ.get("TORNADO_DB"):
        return os.environ["TORNADO_DB"]
    return PROD_DB if os.path.exists(PROD_DB) else str(lb.DB_PATH)


def check_owner(path):
    """Refuse to touch the database as another user (see the WAL incident
    described in leaderboard.hold_wal_files)."""
    if not os.path.exists(path):
        sys.exit(f"no database at {path}")
    owner = os.stat(path).st_uid
    if os.geteuid() != owner:
        try:
            import pwd
            who = pwd.getpwuid(owner).pw_name
        except (ImportError, KeyError):
            who = str(owner)
        sys.exit(f"run this as the database owner: sudo -u {who} python3 -B {os.path.realpath(__file__)} ...")


def fmt_time(ts):
    return time.strftime("%Y-%m-%d %H:%M", time.gmtime(ts)) if ts else "?"


def rate(score, secs):
    return f"{score / secs:.0f}" if secs else "-"


def print_rows(rows):
    if not rows:
        print("(none)")
        return
    print(f"{'id':>6}  {'date (UTC)':16}  {'board':16}  {'name':16}  {'score':>9}  "
          f"{'wave':>4}  {'time':>6}  {'pts/s':>6}  {'combo':>5}  device")
    for r in rows:
        board = f"{r['mode']}/{DIFFS.get(r['difficulty'], r['difficulty'])}"
        secs = r["run_time"]
        print(f"{r['id']:>6}  {fmt_time(r['created_ts']):16}  {board:16}  {r['name']:16}  "
              f"{r['score']:>9}  {r['wave']:>4}  {secs // 60:>3}:{secs % 60:02d}  "
              f"{rate(r['score'], secs):>6}  {r['max_combo']:>5}  {(r['player_hash'] or '-')[:12]}")


def confirm(args, what):
    if args.yes:
        return
    if not sys.stdin.isatty():
        sys.exit("refusing without confirmation (use --yes)")
    if input(f"{what} Continue? [y/N] ").strip().lower() not in ("y", "yes"):
        sys.exit("aborted")


def cmd_top(conn, args):
    print_rows(conn.execute(
        "SELECT * FROM scores WHERE mode = ? AND difficulty = ? ORDER BY score DESC, id LIMIT ?",
        (args.mode, args.difficulty, args.n)).fetchall())


def cmd_recent(conn, args):
    print_rows(conn.execute("SELECT * FROM scores ORDER BY id DESC LIMIT ?", (args.n,)).fetchall())


def cmd_show(conn, args):
    r = conn.execute("SELECT * FROM scores WHERE id = ?", (args.id,)).fetchone()
    if r is None:
        sys.exit(f"no score #{args.id}")
    for k in r.keys():
        v = fmt_time(r[k]) + " UTC" if k == "created_ts" else r[k]
        print(f"{k:>12}: {v}")
    if r["player_hash"]:
        names = [x[0] for x in conn.execute(
            "SELECT name_key FROM players WHERE key_hash = ?", (r["player_hash"],))]
        n = conn.execute("SELECT COUNT(*) FROM scores WHERE player_hash = ?",
                         (r["player_hash"],)).fetchone()[0]
        print(f"{'device':>12}: {n} score(s), names claimed: {', '.join(names) or '-'}")


def cmd_delete(conn, args):
    rows = conn.execute(f"SELECT * FROM scores WHERE id IN ({','.join('?' * len(args.ids))})",
                        args.ids).fetchall()
    if not rows:
        sys.exit("no such scores")
    print_rows(rows)
    confirm(args, f"Delete {len(rows)} score(s)?")
    with conn:
        conn.executemany("DELETE FROM scores WHERE id = ?", [(r["id"],) for r in rows])
    print(f"deleted {len(rows)}")


def ban_targets(conn, args):
    """-> (names to ban, player hashes to ban)"""
    names, players = set(), set()
    if args.name:
        name = args.name.strip()
        if name.upper() == lb.ANON:
            sys.exit("ban a device instead: --score ID of one of its anonymous scores")
        names.add(name.lower())
        owner = conn.execute("SELECT key_hash FROM players WHERE name_key = ?",
                             (name.lower(),)).fetchone()
        if owner:
            players.add(owner[0])
        players.update(r[0] for r in conn.execute(
            "SELECT DISTINCT player_hash FROM scores WHERE lower(name) = ? AND player_hash != ''",
            (name.lower(),)))
    else:
        r = conn.execute("SELECT name, player_hash FROM scores WHERE id = ?", (args.score,)).fetchone()
        if r is None:
            sys.exit(f"no score #{args.score}")
        if r["name"] != lb.ANON:
            names.add(r["name"].lower())
        if r["player_hash"]:
            players.add(r["player_hash"])
        if not names and not players:
            sys.exit("that score predates device tracking and is anonymous: delete it instead")
    # Every name those devices claimed goes too.
    for p in list(players):
        names.update(x[0] for x in conn.execute("SELECT name_key FROM players WHERE key_hash = ?", (p,)))
    return names, players


def cmd_ban(conn, args):
    names, players = ban_targets(conn, args)
    q_names = ",".join("?" * len(names)) or "NULL"
    q_players = ",".join("?" * len(players)) or "NULL"
    victims = conn.execute(
        f"SELECT * FROM scores WHERE lower(name) IN ({q_names}) OR player_hash IN ({q_players})"
        " ORDER BY score DESC", (*names, *players)).fetchall()
    print("names:  ", ", ".join(sorted(names)) or "-")
    print("devices:", ", ".join(p[:12] for p in sorted(players)) or "-")
    print_rows(victims)
    keep = args.keep_scores
    confirm(args, f"Ban {len(names)} name(s) and {len(players)} device(s)"
                  f"{'' if keep else f', deleting {len(victims)} score(s)'}?")
    now = int(time.time())
    with conn:
        conn.executemany("INSERT OR REPLACE INTO bans (kind, value, reason, created) VALUES (?,?,?,?)",
                         [("name", n, args.reason, now) for n in names] +
                         [("player", p, args.reason, now) for p in players])
        if not keep:
            conn.executemany("DELETE FROM scores WHERE id = ?", [(r["id"],) for r in victims])
    print(f"banned; {'kept' if keep else 'deleted'} {len(victims)} score(s)")


def cmd_unban(conn, args):
    kind, value = ("name", args.name.strip().lower()) if args.name else ("player", args.player)
    with conn:
        n = conn.execute("DELETE FROM bans WHERE kind = ? AND value = ?", (kind, value)).rowcount
    print(f"removed {n} ban(s)" if n else "no such ban")


def cmd_bans(conn, args):
    rows = conn.execute("SELECT * FROM bans ORDER BY created DESC").fetchall()
    if not rows:
        print("(none)")
    for r in rows:
        print(f"{fmt_time(r['created'])}  {r['kind']:6}  {r['value'][:24]:24}  {r['reason']}")


def cmd_release(conn, args):
    key = args.name.strip().lower()
    if not conn.execute("SELECT 1 FROM players WHERE name_key = ?", (key,)).fetchone():
        sys.exit(f"'{args.name}' is not claimed")
    confirm(args, f"Let the next player who submits as '{args.name}' claim it?")
    with conn:
        conn.execute("DELETE FROM players WHERE name_key = ?", (key,))
    print("released")


def build_parser():
    ap = argparse.ArgumentParser(description="Tornado 3D leaderboard moderation",
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__.split("Commands", 1)[1].split("Destructive")[0])
    ap.add_argument("--db", default=None, help=f"database (default: $TORNADO_DB or {PROD_DB})")
    ap.add_argument("-y", "--yes", action="store_true", help="do not ask for confirmation")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("top")
    p.add_argument("-m", "--mode", choices=lb.MODES, default="classic")
    p.add_argument("-d", "--difficulty", type=int, choices=(0, 1, 2), default=1)
    p.add_argument("-n", type=int, default=20)
    p = sub.add_parser("recent")
    p.add_argument("-n", type=int, default=20)
    p = sub.add_parser("show")
    p.add_argument("id", type=int)
    p = sub.add_parser("delete")
    p.add_argument("ids", type=int, nargs="+")
    p = sub.add_parser("ban")
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("--name")
    g.add_argument("--score", type=int)
    p.add_argument("--reason", default="")
    p.add_argument("--keep-scores", action="store_true")
    p = sub.add_parser("unban")
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("--name")
    g.add_argument("--player")
    sub.add_parser("bans")
    p = sub.add_parser("release")
    p.add_argument("name")
    return ap


def main(argv=None):
    args = build_parser().parse_args(argv)
    path = args.db or default_db()
    check_owner(path)
    lb.DB_PATH = Path(path)
    lb.init_db()                       # applies pending migrations (bans table, ...)
    conn = lb.connect()
    conn.row_factory = sqlite3.Row
    conn.isolation_level = "DEFERRED"  # use `with conn:` transactions
    try:
        globals()["cmd_" + args.cmd](conn, args)
    finally:
        conn.close()


if __name__ == "__main__":
    main()
