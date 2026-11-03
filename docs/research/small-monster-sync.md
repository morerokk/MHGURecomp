# Small monster sync in multiplayer (2026-10-20/21)

Small monsters ("zako") are synced between the players in a zone like large monsters are: positions, movement and
actions. The original game syncs only their HP / deaths; each player's game runs their AI itself, so everyone sees
them elsewhere. Implemented in runtime/net/emsync.c, off by default since 2026-10-27: ini `[Multiplayer]
SyncSmallMonsters = On` (env `MHGU_SYNC_ZAKO=0/1`). Addresses below are runtime addresses (image offset + 0x200000: what
`tools/disasm.py`, `fntrace` and thread dumps use). `tools/retool.py` (strings / xref / callers / dti / vtslot)
finds all of this again.

## How the game syncs monsters

Network layer: MT Framework RPC (`cRemoteCall` messages, `cRemoteProcedure` handlers) in `nFestaNetwork`, over
Nintendo Pia. Large monsters have a *master* player whose game runs the AI; at every new action it sends
`netEmAction` (0xcc bytes: position, rotation, HP, action, area / waypoint state, targets, ailments...) and the
others replay it. Small monsters are spawned per zone from `quest\zako\emSetList\`; only their herds ("Mure":
`netMureEm*`) and HP / deaths are synced.

Small and large monsters are one class family (`uEmsNNN_00` / `uEmNNN_00` -> `uFestaEnemy` -> `uEnemy` ->
`uEnemyBase` -> `uEnemyCharacter`), with the same network code. The difference is data in the status block
(`*(monster + 0x1428)`):

| status block | meaning |
|---|---|
| +0x40 | position (3 floats) |
| +0x1a8 | flags: 8 = master (its actions are sent), 0x80 = simulated locally (all small monsters, always) |
| +0x1b6 | u16 network id: large monsters from the quest's set (e.g. 62), small monsters 10000 + a quest-wide spawn index |
| +0x1e4 | HP (float) |

- Send: `0x2cafe8(monster, kind, data)` (callers check flag 8 first) -> per-kind vtable slot (action:
  +0x42c = `0x2cb330`, fills netEmAction, sends via `0x3eb810`).
- Receive: netEmActionProcedure (`0x3f4554`) -> enemy manager (`sEnemy`, global `0x1a51fe0`) `0x265b14` ->
  find by id among its 10 live enemies (+0x1c..+0x40; small monsters included) -> per-monster receive `0x2cb120`
  (drops own messages, dispatches by kind to the vtable slots +0x430...; action: `0x2cc154` copies the state in).
- Small monsters already *receive* fine; they never send because nothing sets flag 8 on them, and their 0x80
  makes every game run them itself. Setting 8 on one player's copies and clearing 0x80 on the others' is enough:
  the others' copies then replay the master's actions (tested with poke before writing any code).

Other data used: `sPlayer` (global `0x1a85b58`): +0x1c the 4 player slots; a player's current zone is the byte at
player + 0x1054 (0..12; setter: uPlayer vtable +0xa4; other players' zones arrive by netPlAreaMove). The local
player's slot: `*(0x1a8ca48) + 0x30`. Area 0 = base camp, 255 = not in a quest.

## The port's part (emsync.c)

- Each frame (from the game's input polling, on its thread, ~30 times a second): every player's zone, debounced
  (a new value counts after 0.7 s); per zone one player runs the small monsters: the first one there. Claims go to
  the other games over the local play link (`F_AUX` frames, ldn_send_aux) on every change and twice a second.
  Two claims for one zone (arrived together): the lower slot keeps it.
- Master: flags 8 | 0x80 on the zone's small monsters. Others in the zone: 0x80 and 8 cleared. Alone in a zone:
  as the original game (0x80, no 8).
- tools/gen.py PATCHES `0x2cb120` calls `emsync_filter`: drops a small monster message on the master's game, and
  one from a player in another zone.

## Tests (two instances on one PC, Harvest Tour: Jurassic Frontier, zone 1's three Aptonoth)

- Before: same ids, positions hundreds of units apart on the two games.
- After: within 0-10 units (centimetres) while walking and grazing, for 5+ minutes; the second player's freshly
  spawned copies snap to the master's within ~4 s of arriving. Either player first in the zone works.
- Hand-over: the master leaves -> the other player's game runs them; comes back -> follows.
- Seen once before the debounce: the master role flipped between two players standing still (a passing read of
  the zone, most likely); not seen again in 6 minutes with the debounce.
- Not tested: combat (hits by a follower, deaths, carving), aggressive small monsters (Jaggi...), 3-4 players,
  two PCs.

## Open problems (2026-11-03)

- Fixed: players dropped in some zones with sync on, e.g. Volcanic Hollow zone 7. uEms100 (Volcanic Hollow zones 2, 3, 7
  of Harvest Tour: Volcano) never gets past its first action: `act` stays 0, no target, and it doesn't move. It starts
  0:0 again every update, twice (from 0x2752d4 and 0x2755a8): about 120 starts a second per monster at 60 fps, 4x that at
  240 fps. Harmless in the original game; with flag 8 each start would send a netEmAction. Reproduced with two instances
  (windowed, [Multiplayer] Server = Off, hub -> Low 3 star Harvest Tour: Volcano, zone 1 -> 2 -> 3 -> 7): the host left
  zone 3 for zone 7, the joiner became master of zone 3, and it left the hub after 576 more starts; before that the other
  hunter was seen jumping back. emsync_tick now leaves a monster whose action word is 0 as the game made it. Checked:
  the same route stayed connected, over 40 s with both hunters in zone 7 (host master; ~10000 uEms100 starts);
  uEms100 at the same place on both games, uEms006 still synced (8 and 222 units apart, sampled once while moving). Not
  checked: that the starts were what went over the network (no packet count; the disconnect going away is the evidence),
  240 fps, 3-4 players, two PCs. A monster stuck looping a later action wouldn't be caught.
- "Small monsters stop attacking": in solo, Maccao preferring Palicoes comes from their score table (docs/STATUS.md,
  "Monsters' attention scores"), not from sync. Whether sync adds to it (reported "looks at the player, then stops") is
  not tested.
- Herds: sEnemy (0x1a51fe0, class sEnemyMure) +0x25c0 holds 10 herds of 0x50 bytes: +0 type, +1 id, +2 the herd's master
  slot, +0xf target type (0 hunter, 1 Palico, 2 enemy, 0xff none), +0x10 target slot, +0x14 zone, +0x18 leader. Members
  copy the herd's target (a monster's own target type is one higher: 1 hunter, 2 Palico, 3 enemy). emsync.c doesn't touch
  herds, so their own master and netMureEm* messages run alongside it.

## Debug tools made for this

- `MHGU_EMTRACE_ZAKO=1`: small monsters' new actions, and twice a second their distance, targets, attention scores and
  rates, plus each species' score tables (runtime/hle/emtrace.c).

- Control file (MHGU_CONTROL): `peek <hex addr> [words]`, `poke <hex addr> <hex word>`,
  `memsave <hex addr> <hex bytes> <file>`, `fntrace <lo> <hi> [n]` (log calls of guest functions in [lo, hi)),
  stick diagonals `STICK_UPLEFT` etc.
- `MHGU_DUMP_THREADS=<s>`: thread dumps with mutex owners and callers.
