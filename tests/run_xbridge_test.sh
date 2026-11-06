#!/bin/sh
# Mixed hub: a hub hosted on a Ryujinx LDN server that a Ryujinx-side player AND a room player join (runtime/net/xbridge.c).
# Stub servers by default; LDN_SERVER=host:port and ROOM_SERVER=path to a room server program use real ones. No game needed.
set -u
cd "$(dirname "$0")/.."
mkdir -p obj/xb-test
FLAGS="-O1 -g -Wall -Wno-unused-function -Iruntime -Ithird_party/enet/include -D_FILE_OFFSET_BITS=64 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_CRT_RAND_S -D_POSIX_THREAD_SAFE_FUNCTIONS"
E=third_party/enet
ENET="$E/callbacks.c $E/compress.c $E/host.c $E/list.c $E/packet.c $E/peer.c $E/protocol.c $E/win32.c"
O=obj/xb-test
clang $FLAGS tests/test_xbridge.c runtime/net/xbridge.c runtime/net/ryuldn.c runtime/net/room.c $ENET -lws2_32 -lwinmm -lzstd -pthread -o $O/test_xbridge.exe || exit 1
clang $FLAGS tests/test_ryuldn.c runtime/net/ryuldn.c -lws2_32 -pthread -o $O/test_ryuldn.exe || exit 1
clang $FLAGS tests/test_room.c runtime/net/room.c $ENET -lws2_32 -lwinmm -lzstd -pthread -o $O/test_room.exe || exit 1
clang $FLAGS tests/emu_room.c runtime/net/room.c $ENET -lws2_32 -lwinmm -lzstd -pthread -o $O/emu_room.exe || exit 1
PS=${PS:-30491}; RP=${RP:-24891}
SERVER=${LDN_SERVER:-127.0.0.1:$PS}
if [ -z "${LDN_SERVER:-}" ]; then
    python tests/ryuldn_stub_server.py $PS > $O/server.log 2>&1 &
    SRV=$!
fi
if [ -n "${ROOM_SERVER:-}" ]; then "$ROOM_SERVER" $RP > $O/room.log 2>&1 & else $O/test_room.exe server $RP > $O/room.log 2>&1 & fi
RS=$!
trap 'kill ${SRV:-} $RS 2>/dev/null' EXIT
sleep 2
fail=0
$O/test_xbridge.exe $SERVER 127.0.0.1:$RP 40 > $O/host.log 2>&1 &
H=$!
sleep 4
EMU_HOLD=9 $O/emu_room.exe join 127.0.0.1:$RP > $O/room_player.log 2>&1 &
RM=$!
sleep 4
$O/test_ryuldn.exe join $SERVER > $O/ryu_player.log 2>&1 || { echo "Ryujinx-side player FAILED"; fail=1; }
wait $RM || { echo "room player FAILED"; fail=1; }
wait $H || { echo "bridge host FAILED"; fail=1; }
for f in host ryu_player room_player; do echo "--- $f"; cat $O/$f.log; done
[ $fail = 0 ] && echo "ALL PASSED" || echo "SOMETHING FAILED"
exit $fail
