# Security

## Reporting a vulnerability

Please report security problems privately through GitHub: **Security → Report a
vulnerability** on this repository. Do not open a public issue with details.
Include what you found, how to reproduce it and what an attacker could do with it.
You should get an answer within a week.

In scope: the site at https://tornado.micutu.com, the leaderboard API under `/api/`,
and the code and deployment files in this repository. Please do not run load tests or
automated scanners against production, and do not submit fake scores beyond what is
needed to demonstrate a problem.

## What the deployment does

**Browser**
- Content-Security-Policy without `'unsafe-inline'`: scripts and styles only from the site
  itself (`'wasm-unsafe-eval'` permits WebAssembly compilation, not `eval`); a per-request
  nonce for the script Cloudflare injects; `object-src`, `base-uri`, `form-action` and
  `frame-ancestors` all `'none'`
- HSTS, `nosniff`, `X-Frame-Options: DENY`, COOP/CORP `same-origin`, a restrictive
  Permissions-Policy, on every response including errors and static assets
- No cookies; the page never renders API data as HTML

**Leaderboard API** (`server/leaderboard.py`)
- Scores need a one-time run token issued by the server at run start, are checked against
  the elapsed server time and the game's scoring rules, and burn the token when rejected
- Names are bound to a per-device secret (only its SHA-256 is stored) and filtered
- Per-client token-bucket limits (IPv6 grouped by /64) in the service plus an nginx flood
  guard; the client address is taken only from the local proxy
- Strict input validation, 2 KiB request bodies (4 KiB cap at nginx), parameterised SQL,
  JSON-only responses
- Cross-origin browser POSTs are refused (Origin / Sec-Fetch-Site)

Anything computed on the player's machine can be faked by a determined player. The checks
above make forging scores expensive and visible, not impossible.

**Server**
- The origin answers only to Cloudflare and the local tunnel; everything else gets 403
- The API runs as an unprivileged user under systemd sandboxing: read-only filesystem except
  its database directory, no capabilities, loopback-only networking, syscall filter,
  memory/CPU/task limits
- Releases are root-owned and immutable; deploys swap a symlink atomically and roll back
  on a failed smoke test
- Daily verified database backups (integrity-checked twice, SHA-256), kept for 14 days

**Supply chain**
- C++ dependencies are release tarballs pinned by SHA-256
- CI uses a read-only token and actions pinned to commit SHAs; the browser test uses a
  locked Playwright version; Dependabot proposes updates
