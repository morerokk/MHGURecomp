# Internet hubs without port forwarding (planned 2026-10-25; steps 2-3 done 2026-10-29, step 4 2026-11-08)

Status: the relay / lobby server (server/) and its client in runtime/net/ldn.c work (docs/STATUS.md "Internet hubs
through a hub server"). Friends are grouped by a matchmaking key ([Multiplayer] Key) rather than a room code: same
key, same hub list; no key, the public list. Direct UDP by hole punching (step 4) is in, with the relay as the
fallback and ini Direct = Off as the relay-only option (STATUS.md has how and what was measured). Still open: step 1
(traffic), the players' side of the audit below. The public server runs (docs/STATUS.md).

Goal: Local Play hubs over the internet that nobody has to forward a port for, for friends (private, by code) and
for public play (a hub list), through a relay / lobby server of our own. Not Ryujinx's servers and no crossplay:
a hub with Ryujinx players can't use the port's own features (small monster sync, HP scaling, Village quests in
the Hub), and a hub can't know in advance that no Ryujinx player will join, so mixing is left out on purpose. Our
own server only lists hubs of a compatible port version. Ryujinx's LDN server (github.com/tsrberry/ryujinx-ldnserver,
MIT) is a reference for the lobby side (listing, join / leave, passphrases); code taken from it goes in
THIRD_PARTY_NOTICES.

## Why it fits

Local Play over IP (runtime/net/ldn.c, STATUS "Local Play over IP") is already a star: every station keeps one TCP
link to the host, which assigns the virtual addresses and relays packets; game packets go by UDP directly when that
gets through. The only reason to forward a port is that the host accepts connections. With a relay, the host and
the stations all connect out to the server, which pipes the same link frames; Scan merges the server's hub list
into the LAN results, so the game's own Search Hubs screen shows internet hubs.

## Plan

1. Measure Local Play's traffic per player (idle in the hub, in a hunt, 2-4 players): sizes the relay's costs.
2. Relay-only server + client: the host registers its hub (NetworkInfo / advertise data, port protocol version),
   friends join by a short code (where they type it: ini, or a panel in the overlay). `[Multiplayer] Server =` in
   the ini with a default; the server open source and easy to self-host.
3. Public hub list: which hubs Search Hubs gets (the SDK's Scan returns a limited number): fullest / newest, same
   version only.
4. Direct UDP by hole punching through the server's help, relay as the fallback (symmetric / carrier-grade NAT).
   Direct means players see each other's IPs: keep a relay-only option.
   Done 2026-11-08: the server tells each end the other's public UDP address only when both asked (ini Direct on
   both: the consent), the host punches and the station's hellos find the way, and either end goes back to the
   relay after 3.5 s without two-way proof. Only host <-> station links (the star): stations still reach each other
   through the host, as on a LAN. Two players behind one router (same public address) would need hairpin NAT for
   it, but they find the hub by LAN search anyway.

## Security (a requirement, not an afterthought)

The relay server:
- Everything a client sends is untrusted: cap frame sizes (the link's u32 size field), buffers and message rates per
  connection; limit connections and hubs per IP; time out idle and half-open connections (slow senders included).
- Not an open proxy: it only forwards between members of one hub, never to an address a client names.
- No amplification: any UDP answer to an unverified source is no bigger than the request; relay UDP only after a
  handshake token.
- Room codes long enough not to be guessed, guessing rate-limited; private hubs never in the public list.
- Hub names / advertise data length-capped and checked before they're listed.
- A memory-safe language for the server (Go or Rust); no admin interface on the public port; keep IPs no longer
  than needed.

Players (the port): today only friends connect (the host's address is typed in), so this matters more once strangers
can join. Audit before public hubs, and is worth doing anyway:
- Everything parsed from the network in the port's own code: ldn.c link frames (sizes, types, NetworkInfo, node
  numbers), vsock.c messages, emsync.c and the hub host's settings (hpscale.c, villagehub.c: values range-checked
  before use). Bounds-check everything, fuzz the parsers.
- The game's own network code (Pia, MT Framework's session layer) parses packets from other players and was written
  for Nintendo's walled garden; a bug there could let a hostile player write guest memory. Guest addresses can't
  reach host memory outside the 4 GB guest window (G2H takes 32 bits), so an escape to the PC would need a bug in
  the port's host side reached from guest code: IPC buffer sizes, the fs service's paths (no `..` out of the game /
  save folders), HLE functions trusting guest lengths. Audit those as the boundary.
- Cheats and hacked saves can't be ruled out in public hubs (open-source client); at most a hub can advertise
  that it runs none.
