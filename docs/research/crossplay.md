# Crossplay with emulators (2026-11-05)

Local Play (nn::ldn HLE, `runtime/net/ldn.c`) can be shared with emulator players. Built into the port, off until configured.

- **Ryujinx / Ryubing**: they use an LdnServer (RyuLdn protocol, port 30456). `runtime/net/ryuldn.c` is a RyuLdn client.
  `RyuServers` makes the lobbies on those servers searchable (they show up in Search Hubs as hubs); `RyuHost` makes the hubs you
  create on that server, so Ryujinx players can join them. Ryujinx's own LDN setting must name the same server.
- **suyu, yuzu and forks (Eden, Citron...)**: they use a multiplayer room (ENet) and their LANDiscovery over it.
  `runtime/net/room.c` is a room member; `RoomServer` joins the room and lists its hubs, `RoomHost = On` offers your hubs in it.
- **Mixed hub** (`runtime/net/xbridge.c`): `RyuHost` and `RoomHost` together. The hub lives on the Ryujinx server; every room
  player that connects joins it as one more RyuLdn client (`ryu_peer_*`), so Ryujinx players, room players and the host share it.
  Room players keep room addresses (192.168.x.y: the host is this process's room address, a Ryujinx player 192.168.250.<node>),
  and the addresses and MACs a game writes into its datagrams are rewritten between the two views (as the earlier suyu-based
  recompiler's room bridge did). Packets between two room players never pass through the bridge.
- The port's extras (small monster sync, HP scaling) are off in crossplay hubs: only port players speak them.

## Tests (no game)

`tests/run_ryuldn_test.sh`, `tests/run_room_test.sh`, `tests/run_xbridge_test.sh` (mixed hub: a Ryujinx-side player and a room
player join the same hub, both get an answer, the host sees three players). They run against stand-in servers
(`tests/ryuldn_stub_server.py`, `test_room server`); the xbridge test also ran against the real Ryubing LdnServer (.NET 9, no
Redis) and the real suyu room server code (`LDN_SERVER=host:port ROOM_SERVER=path sh tests/run_xbridge_test.sh`).

## Not verified

- Not run in the game, not with real Ryujinx or a current suyu / Eden program.
- MHGU's local communication id is taken as its title id 0x0100770008DD8000.
- A room player's unicast packet to a *Ryujinx* player goes to a stand-in address that no room member owns: a stock room server
  drops it (the old suyu-based recompiler's own room handed such addresses to a gateway). Broadcasts, and everything between a
  room player and the host or another room player, work. If MHGU needs unicast between non-host players, it will show in play.
- Node numbers: the server numbers Ryujinx players, the port's hub numbers its own; they match while nobody leaves and rejoins.
