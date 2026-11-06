#!/bin/sh
# Builds tests/test_room.c (room.c + ENet) and runs a stand-in room server with two members. No game needed.
set -e
cd "$(dirname "$0")/.."
mkdir -p obj/room-test
FLAGS="-O1 -g -Wall -Wno-unused-function -Iruntime -Ithird_party/enet/include -D_FILE_OFFSET_BITS=64 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_CRT_RAND_S -D_POSIX_THREAD_SAFE_FUNCTIONS"
E=third_party/enet
clang $FLAGS tests/test_room.c runtime/net/room.c runtime/net/roomsrv.c $E/callbacks.c $E/compress.c $E/host.c $E/list.c $E/packet.c $E/peer.c $E/protocol.c $E/win32.c -lws2_32 -lwinmm -lzstd -pthread -o obj/room-test/test_room.exe
PORT=${PORT:-24899}
obj/room-test/test_room.exe ${SERVER:-server} $PORT > obj/room-test/server.log 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT
sleep 1
obj/room-test/test_room.exe a 127.0.0.1:$PORT > obj/room-test/a.log 2>&1 &
A=$!
sleep 1
obj/room-test/test_room.exe b 127.0.0.1:$PORT > obj/room-test/b.log 2>&1 || echo "B FAILED"
wait $A || echo "A FAILED"
for f in server a b; do echo "--- $f"; cat obj/room-test/$f.log; done
