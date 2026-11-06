# MHGURecomp hub server

The lobby / relay server for internet hubs: MHGURecomp players find each other's Local Play hubs through it and
play together without forwarding a port. Like an LDN server for Ryujinx, but for this port only (it lists MHGURecomp
hubs of the same port version, no Ryujinx players).

- A hub created in the game is listed here for players with the same **matchmaking key** (`[Multiplayer] Key` in
  `mhgurecomp.ini`, or the settings menu). Friends who all enter the same key only see each other's hubs; without a
  key a hub is public and seen by everyone else without a key. The game sends a hash of the key, never the key.
- Joining goes through the server: it pipes the player's connection to one the host opens for them, and passes the
  game's UDP packets between the two. Nobody needs a forwarded port, and players never see each other's IP address.
- Players on the same LAN still find each other directly (LAN search comes first).

One small executable, no database, no configuration file. Uses one port, TCP and UDP (default 39510).

## Build

Go 1.22 or newer (https://go.dev/dl/), nothing else:

```
cd server
go build -o mhgurecomp-hubserver .
go test ./...
```

`GOOS=linux GOARCH=amd64 go build -o mhgurecomp-hubserver .` builds a Linux server on any OS (also `arm64`).

## Run

```
./mhgurecomp-hubserver -addr :39510
```

Open the port for **TCP and UDP** in the firewall (and forward it, if the server is behind a router). Options:

| Flag | Default | |
|---|---|---|
| `-addr` | `:39510` | address and port (TCP and UDP) |
| `-max-conns` | 4000 | connections at once |
| `-max-conns-per-ip` | 24 | connections at once from one IP address |
| `-max-hubs` | 2000 | hubs listed at once |
| `-max-hubs-per-ip` | 4 | hubs from one IP address |
| `-list-max` | 18 | hubs in one search answer, as many as the game's Join Hub list shows (3 pages of 6). With more hubs than that, each search gets a random choice of them (hubs with a free place before full ones), so a Retry shows others. The answer is sorted: free places first, then the fullest, then the newest |
| `-bandwidth` | 1048576 | bytes a second per player link and direction (a hub uses far less) |
| `-min-proto` | 0 | the oldest link protocol (the game's `PROTO_VERSION`, runtime/net/ldn.c) served: older games are refused, and they say "The hub server runs another version" and show only LAN / Join hubs. `-min-proto 3` refuses 0.14.2 and older. 0: any version. Restart the server to apply it: hubs already up stay until they disconnect |
| `-v` | off | log hubs and joins (never IP addresses) |

With Docker: `docker build -t mhgurecomp-hubserver server` and
`docker run -d --restart unless-stopped -p 39510:39510/tcp -p 39510:39510/udp mhgurecomp-hubserver`.

As a systemd service (Linux), `/etc/systemd/system/mhgurecomp-hubserver.service`:

```
[Unit]
Description=MHGURecomp hub server
After=network-online.target

[Service]
ExecStart=/usr/local/bin/mhgurecomp-hubserver -addr :39510
DynamicUser=yes
Restart=always

[Install]
WantedBy=multi-user.target
```

then `systemctl enable --now mhgurecomp-hubserver`.

## Using it in the game

`[Multiplayer] Server = your.host.name` (or `host:port` for another port) in `mhgurecomp.ini`, or "Hub server" in the
settings menu (Multiplayer page). Empty means the default server built into the game, `Off` no internet hubs.
Everyone who wants to see each other's hubs must use the same server.

## Protocol

server.go's header comment. In short: control frames (u32 size, u8 type, payload) on TCP; requests LIST, HOST,
JOIN, ATTACH carry "MHGS", the server protocol version, the port's link protocol version and the key's hash. After a
join the two connections are spliced and carry the games' own link (runtime/net/ldn.c), which the server does not
read. UDP datagrams "MHGR" + pair id + the sender's secret + payload are passed, without that prefix, to the other end
of the pair.

Safety: sizes capped, every NetworkInfo checked before it is listed, LIST / JOIN rate limited per IP address,
connections and hubs per IP address limited, handshakes time out, the relay only forwards between the two ends of a
pair it made (never to an address a client names) and a relayed datagram is always smaller than the one received.
