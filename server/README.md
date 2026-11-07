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
| `-min-proto` | 0 | the oldest link protocol (the game's `PROTO_VERSION`, runtime/net/ldn.c) served: older games are refused, and they say "The hub server runs another version" and show only LAN / Join hubs. `-min-proto 3` refuses 0.14.2 and older. 0: any version. Can change while the server runs (`-settings`) |
| `-separate-phones` | on | Android players without a matchmaking key get public hubs of their own: the game sends the key `public-phone` then, and this server keeps that room apart, so phones and PCs don't see each other's public hubs. `-separate-phones=false` makes it the public room: everyone without a key sees everyone's hubs. Keyed rooms are not affected either way. Can change while the server runs (`-settings`) |
| `-settings` | none | a settings file for the two above, read at the start and again on SIGHUP: see below. Its values win over the flags'; a setting it leaves out keeps the flag's |
| `-v` | off | log hubs and joins (never IP addresses): each hub with its link protocol and room, and hubs refused by `-min-proto` |

### Changing settings while the server runs

`-min-proto` and `-separate-phones` can change without a restart, so no hub or game is dropped. Put them in a file:

```
# /etc/mhgurecomp-hubserver.conf
min-proto = 3          # a number; 0: any version
separate-phones = on   # on / off
```

start the server with `-settings /etc/mhgurecomp-hubserver.conf`, and after editing the file send it SIGHUP
(`systemctl reload mhgurecomp-hubserver` with the unit below, or `kill -HUP <pid>`). The log says what changed. A
file that can't be read or has a mistake (an unknown key, a bad value) changes nothing and the log says why; at the
start it stops the server instead. Windows has no SIGHUP: there the file is only read at the start.

- A higher `min-proto` refuses older games from the next request on and takes their hubs off the list: the server
  closes those hosts' connections, and their games try again and say the server runs another version. Players already
  in such a hub play on through the relay until they leave it. A lower one lets those games back in at once.
- `separate-phones` applies to every search and join from then on, to the hubs already listed too: nobody reconnects.

With Docker: `docker build -t mhgurecomp-hubserver server` and
`docker run -d --restart unless-stopped -p 39510:39510/tcp -p 39510:39510/udp mhgurecomp-hubserver`.

As a systemd service (Linux), `/etc/systemd/system/mhgurecomp-hubserver.service`:

```
[Unit]
Description=MHGURecomp hub server
After=network-online.target

[Service]
ExecStart=/usr/local/bin/mhgurecomp-hubserver -addr :39510 -settings /etc/mhgurecomp-hubserver.conf
ExecReload=/bin/kill -HUP $MAINPID
DynamicUser=yes
Restart=always

[Install]
WantedBy=multi-user.target
```

then `systemctl enable --now mhgurecomp-hubserver`. Create `/etc/mhgurecomp-hubserver.conf` first (empty is fine: the
flags' values), or leave `-settings` and `ExecReload` out if you don't need to change settings while it runs.

## Using it in the game

`[Multiplayer] Server = your.host.name` (or `host:port` for another port) in `mhgurecomp.ini`, or "Hub server" in the
settings menu (Multiplayer page). Empty means the default server built into the game, `Off` no internet hubs.
Everyone who wants to see each other's hubs must use the same server.

### The log's rooms

With `-v`, a hub's line names its room: `public` (no matchmaking key), `phone` (Android without a key: the key
`public-phone`, listed with `public` while `separate-phones` is off) or `key` and 32 hex digits, the key's hash as the game sends it (the server never sees a key). To
check whether a room is a key you can guess, hash the guess the way the game does and compare:

```
python3 -c "import hashlib,sys; print(hashlib.sha256(b'MHGURecomp matchmaking key:' + sys.argv[1].strip(' ').encode()).hexdigest()[:32])" "the key"
```

## Protocol

server.go's header comment. In short: control frames (u32 size, u8 type, payload) on TCP; requests LIST, HOST,
JOIN, ATTACH carry "MHGS", the server protocol version, the port's link protocol version and the key's hash. After a
join the two connections are spliced and carry the games' own link (runtime/net/ldn.c), which the server does not
read. UDP datagrams "MHGR" + pair id + the sender's secret + payload are passed, without that prefix, to the other end
of the pair.

Safety: sizes capped, every NetworkInfo checked before it is listed, LIST / JOIN rate limited per IP address,
connections and hubs per IP address limited, handshakes time out, the relay only forwards between the two ends of a
pair it made (never to an address a client names) and a relayed datagram is always smaller than the one received.
