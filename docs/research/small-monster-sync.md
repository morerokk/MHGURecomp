# Small monster sync in multiplayer (2026-10-20/21)

Small monsters ("zako") are synced between the players in a zone like large monsters are: positions, movement and
actions. The original game syncs only their HP / deaths; each player's game runs their AI itself, so everyone sees
them elsewhere. Implemented in runtime/net/emsync.c, on by default: ini `[Multiplayer] SyncSmallMonsters = On`
(env `MHGU_SYNC_ZAKO=0/1`). Addresses below are runtime addresses (image offset + 0x200000: what
`tools/disasm.py`, `fntrace` and thread dumps use). `tools/retool.py` (strings / xref / callers / dti / vtslot)
finds all of this again.

## How the game syncs monsters

Network layer: MT Framework RPC (`cRemoteCall` messages, `cRemoteProcedure` handlers) in `nFestaNetwork`, over
Nintendo Pia. Large monsters have an *owner* player whose game runs the AI; at every new action it sends
`netEmAction` (0xcc bytes: position, rotation, HP, action, area / waypoint state, targets, ailments...) and the
others replay it. Small monsters are spawned per zone from `quest\zako\emSetList\`; only their herds ("Mure":
`netMureEm*`) and HP / deaths are synced.

Small and large monsters are one class family (`uEmsNNN_00` / `uEmNNN_00` -> `uFestaEnemy` -> `uEnemy` ->
`uEnemyBase` -> `uEnemyCharacter`), with the same network code. The difference is data in the status block
(`*(monster + 0x1428)`):

| status block | meaning |
|---|---|
| +0x40 | position (3 floats) |
| +0x1a8 | flags: 8 = networked (actions are sent, on every game: a large monster has it on all of them); the rest is per species |
| +0x1b6 | u16 network id: large monsters from the quest's set (e.g. 62), small monsters 10000 + a quest-wide spawn index |

Who runs a monster is on the monster itself: +0x7420 (u8) this game runs its AI, +0x7424 (u8) the owner's slot. Seen on large
monster 62 with two players: flags 0x10600049 on both games, +0x7420 1 on the host and 0 on the other, +0x7424 0 on both. The AI's
next action (0x275510) starts only with +0x7420 set. 0x2b16e0 (once per monster, +0x7423) gives a networked monster to the
session host.

Flags per species: table behind 0x26b3fc (species id at monster +0xb5f0, 0x1000 + the ems number). 0x80 in it: ems001, 004, 005,
014, 027, 047; uEms007 shows it too (not in the table). Without: ems002, 003, 006, 008, 022, 023, 025, 026, 046... A monster with
0x80 keeps no attention scores (they stay 0, measured with hunters near). Species names below are from the voice and sound file
names and from screenshots: ems003 Kelbi, ems023 Jaggi, ems025 Rhenoplos (seen).

- Send: `0x2cafe8(monster, kind, data)` (callers check flag 8 first; a replayed action, mode 4, sends nothing) -> per-kind vtable slot (action:
  +0x42c = `0x2cb330`, fills netEmAction, sends via `0x3eb810`).
- Receive: netEmActionProcedure (`0x3f4554`) -> enemy manager (`sEnemy`, global `0x1a51fe0`) `0x265b14` ->
  find by id among its 10 live enemies (+0x1c..+0x40; small monsters included) -> per-monster receive `0x2cb120`
  (drops own messages, dispatches by kind to the vtable slots +0x430...; action: `0x2cc154` copies the state in).
- Small monsters already *receive* fine; they never send because nothing sets flag 8 on them, and +0x7420 is 1 on every
  game, so every game runs them itself.

Other data used: `sPlayer` (global `0x1a85b58`): +0x1c the 4 player slots; a player's current zone is the byte at
player + 0x1054 (0..12; setter: uPlayer vtable +0xa4; other players' zones arrive by netPlAreaMove). The local
player's slot: `*(0x1a8ca48) + 0x30`. Area 0 = base camp, 255 = not in a quest.

## The port's part (emsync.c)

- Each frame (from the game's input polling, on its thread, ~30 times a second): every player's zone, debounced
  (a new value counts after 0.7 s); per zone one player owns the small monsters: the first one there. Claims go to
  the other games over the local play link (`F_AUX` frames, ldn_send_aux) on every change and twice a second.
  Two claims for one zone (arrived together): the lower slot keeps it.
- With others in the zone, on every game: flag 8, +0x7424 the owner's slot, +0x7420 1 on the owner's game and 0 on the others'
  (+0x7423 set: 0x2b16e0 would hand it to the session host). Alone in a zone: as the original game (no 8, +0x7420 1). The
  species' other flags are never touched. Monsters of another zone still in the list stay as the game made them.
- tools/gen.py PATCHES `0x2cb120` calls `emsync_filter`: drops a small monster message on the owner's game, and
  one from a player in another zone.

## Tests (two instances on one PC, Harvest Tour: Jurassic Frontier, zone 1's three Aptonoth)

- Before: same ids, positions hundreds of units apart on the two games.
- After: within 0-10 units (centimetres) while walking and grazing, for 5+ minutes; the second player's freshly
  spawned copies snap to the owner's within ~4 s of arriving. Either player first in the zone works.
- Hand-over: the owner leaves -> the other player's game runs them; comes back -> follows.
- Seen once before the debounce: the owner role flipped between two players standing still (a passing read of
  the zone, most likely); not seen again in 6 minutes with the debounce.
- Not tested: carving, 3-4 players, two PCs.

## Small monsters ignoring hunters with sync on (fixed 2026-11-04)

Reported: on by default, small monsters ignore players in multiplayer. The first version read flag 8 as "master" and 0x80 as
"simulated locally", set 8 | 0x80 on the master's copies and also 0x80 when alone in a zone, and cleared both on the others'. So
every hostile species got 0x80 on the game that ran it (no attention scores: no target, it wanders) and followers kept running their
own AI between messages (+0x7420 still 1), hundreds or thousands of units off for species that run. Passive species lost 0x80 on
followers. Zone 1 of this quest has only ems047 (0x80 by species), which is why the first tests saw nothing wrong.

Test: Harvest Tour: Jurassic Frontier (q0010101). Its unmodded zone 2 has Kelbi and uEms007, which flee or wander whatever happens,
so a scratch mod turned the zone's four into two ems023 (Jaggi) and two ems025 (Rhenoplos, no herd), loaded by the quest from the
list alone. Two instances, Server Off, MHGU_EMTRACE_ZAKO=1 and `MHGU_CONTROL` driving; from base camp: `STICK_UP 60`,
`STICK_UPLEFT 400`, `STICK_LEFT 240` to zone 1, then `STICK_RIGHT 480` twice to zone 2.

- Before (Kelbi, as those were the zone's monsters): master's scores 0 at any distance, target never a hunter: 0 of ~3600 samples.
- Original game (sync off, one hunter, the mod): target the hunter, group 7 (attack) actions within 30 s.
- After (sync on, two hunters, final build): Jaggi and Rhenoplos target both hunters, group 7 starts 15 on both games in about two
  minutes; action starts per monster, group by group, the same on both when added up (owner mode 0 or 5, follower mode 4), none of
  its own on a follower. Positions 0 off when still, 300 at most while running (a move sent at its start, each game walks it). A
  follower's arrows flinch the monster on the owner's game (group 10, replayed); deaths (group 11) replay. Roles the other way round
  and hand-over in the middle of the fight: same. Flags of passive ems047 stay as the game made them.
- Not measured: damage numbers taken by hunters (no HP field read), monster HP on both games, 3-4 players, two PCs, 240 fps.
- Seen once: the owner role moved to the other player while the owner walked near a zone's exit, both still in the zone. The monsters
  carried on (+0x7420 swapped). Likely the same passing read of the zone; not chased.

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
  240 fps, 3-4 players, two PCs. A monster stuck looping a later action wouldn't be caught. Since 2026-11-04 a monster joins the
  sync once its action word has been non-zero (others pass through 0:0 for a moment, so the word alone can't tell); this route not
  run again since.
- Maccao preferring Palicoes comes from their score table (docs/STATUS.md, "Monsters' attention scores"), not from sync: the user
  reports they rarely go for hunters unprovoked. Use Jaggi or Rhenoplos to test hostility.
- Herds: sEnemy (0x1a51fe0, class sEnemyMure) +0x25c0 holds 10 herds of 0x50 bytes: +0 type, +1 id, +2 the herd's master
  slot, +0xf target type (0 hunter, 1 Palico, 2 enemy, 0xff none), +0x10 target slot, +0x14 zone, +0x18 leader. Members
  copy the herd's target (a monster's own target type is one higher: 1 hunter, 2 Palico, 3 enemy). emsync.c doesn't touch
  herds, so their own master and netMureEm* messages run alongside it. 0x264f90 sets a herd's target only on its master's game;
  no harm seen in the tests, where one herd's master and the zone's owner were different players.

## Debug tools made for this

- `MHGU_EMTRACE_ZAKO=1`: small monsters' new actions, and twice a second their distance, targets, attention scores and
  rates, plus each species' score tables (runtime/hle/emtrace.c).

- Control file (MHGU_CONTROL): `peek <hex addr> [words]`, `poke <hex addr> <hex word>`,
  `memsave <hex addr> <hex bytes> <file>`, `fntrace <lo> <hi> [n]` (log calls of guest functions in [lo, hi)),
  stick diagonals `STICK_UPLEFT` etc.
- `MHGU_DUMP_THREADS=<s>`: thread dumps with mutex owners and callers.
