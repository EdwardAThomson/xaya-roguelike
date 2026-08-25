# Deploying the hosted sandbox demo

This describes how to host the roguelike as a public shared sandbox: one
URL, no wallet, anyone picks a name and plays a shared world. It is a demo
sandbox, not a production chain (see Caveats).

## Architecture

This box fronts the sandbox with a Cloudflare Tunnel (`cloudflared`): TLS is
terminated at Cloudflare's edge and the tunnel forwards plain HTTP to Caddy on
localhost:80. Caddy serves the static frontend and reverse-proxies game traffic
to the local move proxy. No inbound port is public; cloudflared dials out.

```
   browser
     |  https
     v
   Cloudflare edge (TLS)
     |  outbound tunnel (cloudflared)
     v
   Caddy (localhost:80)
     |-- /            -> static frontend (index.html, dist/, style.css)
     |-- /gsp         -> move proxy  (relays read-only GSP calls)
     |-- /proxy[/...] -> move proxy  (moves, register, mine, health)
                              |
                              |  localhost only
                              v
                         rogueliked (GSP RPC :18332)
                         anvil + xayax-eth
```

The move proxy (`devnet/frontend_devnet.py`) is the single backend origin: it
submits moves and relays an allowlist of read-only GSP methods, refusing
anything else (notably `stop`). anvil, xayax, the GSP RPC, and Caddy itself all
stay bound to localhost, so none of them are reachable from the internet; only
Cloudflare's edge is public.

If you would rather not use a tunnel, Caddy can terminate TLS directly instead
(see "Alternative: Caddy-managed TLS" below), at the cost of exposing the origin
IP and opening ports 80/443.

## Prerequisites on the host

- Build deps for the GSP (see `docs/SETUP.md`) plus libxayagame, so
  `rogueliked` builds.
- Foundry (`anvil`) on PATH.
- The xayax Python venv (provides `xayax.eth`) and the `xayax-eth` binary
  at `/usr/local/bin/xayax-eth`.
- Caddy (serves the static frontend and routes `/gsp` + `/proxy` on
  localhost:80).
- `cloudflared` (the Cloudflare Tunnel daemon).
- Node + TypeScript to build the frontend.
- A domain on Cloudflare (DNS managed there); the tunnel creates the record.

## 1. Build

Backend (this repo):

```bash
cmake -B build
cmake --build build -j$(nproc)
```

Frontend (`xaya-roguelike-frontend`):

```bash
npx tsc          # compiles src/ -> dist/
```

The frontend auto-selects endpoints: served from a real domain it uses the
same-origin `/gsp` and `/proxy` paths (see `src/config.ts`); served from
localhost it uses the devnet ports directly. No rebuild per host is needed.

## 2. Lay out files

```
/opt/xayaroguelike            # this repo (built)
/opt/xayax/.venv              # xayax venv  (or set ROG_VENV)
/var/www/rog-frontend         # frontend index.html + style.css + dist/
```

## 3. Run the stack

Via systemd (recommended):

```bash
sudo cp devnet/deploy/rog-sandbox.service /etc/systemd/system/
# edit User / paths in the unit if your layout differs
sudo systemctl daemon-reload
sudo systemctl enable --now rog-sandbox
```

Or directly for a quick test:

```bash
ROG_PROJECT=/opt/xayaroguelike ROG_VENV=/opt/xayax/.venv \
  devnet/run_sandbox.sh
```

This brings up anvil + xayax + the GSP + the move proxy on `:18380`, with
a background miner advancing the chain every few seconds.

## 4. Front it with Caddy + a Cloudflare Tunnel

Caddy serves on localhost:80 (the committed `Caddyfile` uses a `:80` site
block); the tunnel provides the public TLS endpoint. Start Caddy:

```bash
sudo cp devnet/deploy/Caddyfile /etc/caddy/Caddyfile   # if using distro caddy
sudo systemctl restart caddy
```

Then create and run the tunnel:

```bash
cloudflared tunnel login                             # authorize your Cloudflare zone
cloudflared tunnel create xayarogue                  # note the printed UUID
cloudflared tunnel route dns xayarogue your-domain   # creates the proxied CNAME
```

If a plain A/AAAA record for the hostname already exists, delete it first or
`route dns` fails with "record with that host already exists".

Put the tunnel config at `/etc/cloudflared/config.yml` (copy the credentials
JSON there too), pointing ingress at Caddy:

```yaml
tunnel: <UUID>
credentials-file: /etc/cloudflared/<UUID>.json

ingress:
  - hostname: your-domain
    service: http://localhost:80
  - service: http_status:404
```

Install it as a service and bring it up last, after Caddy is serving:

```bash
sudo cloudflared service install
sudo systemctl enable --now cloudflared    # look for "Registered tunnel connection"
```

Visit `https://your-domain`, pick a name, and play.

## 5. Lock down inbound

With the tunnel, `cloudflared` only dials out, so nothing needs a public
inbound port. Close everything except SSH:

```bash
sudo ufw default deny incoming && sudo ufw default allow outgoing
sudo ufw allow 22/tcp
sudo ufw enable
```

## 6. Verify the live deployment

- Open the URL in a clean browser; register, discover a segment,
  gate-walk, die, reconnect.
- Confirm state updates flow (the GSP read relay works).
- Confirm the relay refuses control methods:

  ```bash
  curl -s https://your-domain/gsp \
    -H 'content-type: application/json' \
    -d '{"jsonrpc":"2.0","id":1,"method":"stop","params":[]}'
  # -> 403 {"error":"method not allowed: stop"}
  ```

- From off-box, confirm nothing is directly reachable: `curl http://<host-ip>:18380`
  and `curl http://<host-ip>:80` should both time out (only Cloudflare is public).

## Alternative: Caddy-managed TLS (no tunnel)

To skip the tunnel and let Caddy terminate TLS directly: point the domain's
A/AAAA record at the host, replace the `:80` site address in the `Caddyfile`
with your domain, open ports 80/443 in the firewall, and run Caddy. It obtains
and renews the certificate automatically. This exposes the origin IP, so it
suits a single-purpose box better than one hosting other services behind
Cloudflare.

## Periodic reset

anvil holds the entire chain in memory and the sandbox world is meant to be
ephemeral. The systemd unit sets `RuntimeMaxSec=86400`, so the stack
restarts daily, starting a fresh chain (the world resets). Tune or remove
that as you like; a restart always wipes the sandbox world.

## Caveats (state these to players)

- **Restart wipes the world.** The chain is in-memory and resets on
  restart, by design. Do not treat sandbox progress as permanent.
- **Claim-token names, single-key moves (temporary demo auth).** The proxy
  submits every move with one funded dev key, so on-chain there is still no
  real ownership. To stop casual impersonation, the proxy hands the first
  client to register a name a random *claim token* (stored in the browser's
  localStorage) and requires it on every later move for that name, so only
  the browser that registered a name can play it for the session. This is
  proxy-layer only, NOT blockchain-secured, and is enabled on the hosted
  sandbox via `ROG_REQUIRE_CLAIM_TOKEN=1` (default OFF for local dev). It is
  explicitly **temporary**: see the "TEMPORARY DEMO AUTH" comment blocks in
  `devnet/frontend_devnet.py` and the frontend `src/net/moves.ts`, and
  remove the whole mechanism before any real-stakes deployment. The real
  ownership model is the wallet path (`WalletMoveTransport`, MetaMask
  signing), scaffolded but dormant; enabling it plus a public testnet is the
  production route (out of scope here).
- **Rate limiting is light.** A per-IP sliding window blunts spam; it is not
  DoS protection.
