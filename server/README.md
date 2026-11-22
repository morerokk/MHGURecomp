# MHGURecomp hub server

The lobby / relay server for internet hubs: MHGURecomp players find each other's Local Play hubs through it and
play together without forwarding a port. Like an LDN server for Ryujinx, but for this port only (it lists MHGURecomp
hubs of the same port version, no Ryujinx players).

- A hub created in the game is listed here for players with the same **matchmaking key** (`[Multiplayer] Key` in
  `mhgurecomp.ini`, or the settings menu). Friends who all enter the same key only see each other's hubs; without a
  key a hub is public and seen by everyone else without a key. The game sends a hash of the key, never the key.
- Joining goes through the server: it pipes the player's connection to one the host opens for them, and passes the
  game's UDP packets between the two. Nobody needs a forwarded port. When both players allow direct connections
  (`[Multiplayer] Direct`, on by default), the server also tells each game the other's address so their packets can go
  straight between them (lower ping) when their routers let them; the relay stays as the fallback. A player with
  Direct off never has their IP address shown to anyone.
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
| `-list-max` | 18 | hubs in one search answer, as many as the game's Join Hub list shows (3 pages of 6). With more hubs than that, each search gets a random choice of them (hubs with a free place and no passcode first, then passcoded ones with a free place, then full ones), so a Retry shows others. The answer is sorted the same way, then the fullest first, then the newest. Passcoded hubs are only listed when open hubs with a free place leave room for them: groups that want privacy can use a matchmaking key |
| `-list-search-max` | 6 | Hunter Search networks in one search answer, after the hubs. With Hunter Search on, the game hosts a 2-player network for ~10 s between its searches, over and over; the server tells them from hubs (0xFF at advertise byte 0x1C, where hubs have 1, 2-player hubs too) and gives them places of their own, so they never take a hub's place in Join Hub. The game scans for 24: 18 hubs, and 6 for its Hunter Search list. Picked and sorted like the hubs |
| `-bandwidth` | 1048576 | bytes a second per player link and direction (a hub uses far less) |
| `-max-presence` | 20000 | players online at once that the friends list keeps track of (protocol 4). When it is full, new players' friends lists get "busy" until others go offline (90 s after their last request) |
| `-min-proto` | 0 | the oldest link protocol (the game's `PROTO_VERSION`, runtime/net/ldn.c) served: older games are refused, and they say "The hub server runs another version" and show only LAN / Join hubs. `-min-proto 3` refuses 0.14.2 and older. 0: any version. Can change while the server runs (`-settings`) |
| `-max-proto` | 100 | the newest link protocol served: newer ones are refused the same way. 0: no newest. Can change while the server runs (`-settings`) |
| `-link-pool` | 3-4 | the link protocols that play together, `low-high`: their hubs are listed and joined across them (a protocol 4 game sees each hub's protocol in HUBS4 and speaks the lower one). A protocol outside it plays with its own only; `off`: each with its own. When a new game version still plays with the last one, `4-5` (and once 3 is retired, `min-proto 4`) lets them meet without a new server. Can change while the server runs (`-settings`) |
| `-separate-phones` | on | Android players without a matchmaking key get public hubs of their own: the game sends the key `public-phone` then, and this server keeps that room apart, so phones and PCs don't see each other's public hubs. `-separate-phones=false` makes it the public room: everyone without a key sees everyone's hubs. Keyed rooms are not affected either way. Can change while the server runs (`-settings`) |
| `-settings` | none | a settings file for the four above, read at the start and again on SIGHUP: see below. Its values win over the flags'; a setting it leaves out keeps the flag's |
| `-v` | off | log hubs and joins (never IP addresses): each hub with its link protocol and room, and hubs refused by `-min-proto` or `-max-proto` |

### Changing settings while the server runs

`-min-proto`, `-max-proto`, `-link-pool` and `-separate-phones` can change without a restart, so no hub or game is
dropped. Put them in a file:

```
# /etc/mhgurecomp-hubserver.conf
min-proto = 3          # a number; 0: any version
max-proto = 100        # a number; 0: no newest
link-pool = 3-4        # low-high, or off
separate-phones = on   # on / off
```

start the server with `-settings /etc/mhgurecomp-hubserver.conf`, and after editing the file send it SIGHUP
(`systemctl reload mhgurecomp-hubserver` with the unit below, or `kill -HUP <pid>`). The log says what changed. A
file that can't be read or has a mistake (an unknown key, a bad value, a `max-proto` below `min-proto`) changes nothing and the log says why; at the
start it stops the server instead. Windows has no SIGHUP: there the file is only read at the start.

- A higher `min-proto` refuses older games from the next request on and takes their hubs off the list: the server
  closes those hosts' connections, and their games try again and say the server runs another version. Players already
  in such a hub play on through the relay until they leave it. A lower one lets those games back in at once.
  `max-proto` works the same way from the other end: a lower one takes newer protocols' hubs off.
- `link-pool` and `separate-phones` apply to every search and join from then on, to the hubs already listed too:
  nobody reconnects.
- A server older than `link-pool` refuses a file that has it (an unknown key): add it only after updating the server.

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

Direct connections (hole punching): a datagram of exactly that prefix + `MHGQ` + 12 zero bytes asks for the other
end's public UDP address and is not passed on. Once both ends of a pair have asked (each game asks only when its
player has `[Multiplayer] Direct = On`: that is their consent to show their address), the server answers the asker
with `MHGP`, the pair id, the asker's secret, the other end's IPv4 address and port, 2 zero bytes and the pair's
direct key (32 bytes, the query's size); the games mark their direct datagrams with that key. The
games then try to reach each other directly and use the relay whenever that does not work, so a server that is
older than this simply keeps every game on the relay.

### Link protocol 4

MHGURecomp 0.19 speaks link protocol 4, which plays with 3: its hosts accept a protocol 3 player and its players
speak 3 to a protocol 3 host (the link frames are the same). The server keeps 3 and 4 in one pool (`-link-pool`), so
players of either version see and join each other's hubs, and `min-proto 4` can retire 3 once few games still use it.
A game asking under 4 also gets:

- **HUBS4** instead of HUBS: each hub with its link protocol and session code.
- **Session codes**: a protocol 4 host may send its hub's code (8 characters of `0123456789ABCDEFGHJKMNPQRSTVWXYZ`)
  after the NetworkInfo. The host picks it for its network and keeps it when it lists again, so a code someone
  shared keeps working. A code held by another hub is refused (error 6, the host picks another); the same hub
  (session id and BSSID) listing again takes it back from its old listing.
- **FIND** (request 7): header + code. The hub holding that code, whatever its room, as a HUBS4 of 0 or 1 hubs.
- **JOIN with a code**: the hub id followed by its code. With the right code the matchmaking key doesn't have to
  match: a code is an invitation.
- **PRESENCE** (request 8, the friends list): header, the install's secret (32 bytes), its state (0 in the game, 1
  hosting, 2 in a hub; another value is passed on as it came), a code (its hub's, or zeros), the number of friends and
  their friend codes (10 bytes each: the first 64 count), and optionally a name length and the name (UTF-8: the hunter
  name the player's friend requests show, cut to 32 bytes at a whole character, control characters left out). Answer FRIENDS (22): the count, then for each listed friend who is online and lists the
  asker back: its friend code, state and code; then the friend requests: their count, and for each player online
  who lists the asker while the asker doesn't list them (the newest 8): its friend code, name length and name.
  Accepting a request is listing that player back. A friend code is the first 10 bytes of
  SHA-256(`MHGURecomp friend:` + secret). The server keeps only who was online in the last 90 seconds, in memory.
  It never tells a player's state to anyone the player doesn't list.
- Hunter Search networks are told from hubs by room for 2 and 0xFF at advertise byte 0x1C. The 8 bytes at 0x14 are
  the host's account id: the same for every game before protocol 4, each install's own since.

Newer games may send more: every request's bytes after the fields above are ignored (a code of zeros is none), and
PRESENCE takes what is over its limits as far as they go instead of refusing it. So a game can add to a request
without a new server, as long as it copes with this one ignoring the addition.

Safety: sizes capped, every NetworkInfo checked before it is listed, LIST / JOIN / FIND / PRESENCE rate limited per IP address,
connections and hubs per IP address limited, handshakes time out, the relay only forwards between the two ends of a
pair it made (never to an address a client names) and a relayed datagram is always smaller than the one received.
