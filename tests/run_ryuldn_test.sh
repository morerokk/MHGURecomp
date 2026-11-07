#!/bin/sh
# Builds tests/test_ryuldn.c, starts the stub server and runs a host and a joiner against it. No game needed.
# Needs MSYS2 UCRT64 (clang) on PATH and python3.
set -e
cd "$(dirname "$0")/.."
mkdir -p obj/ryuldn-test
FLAGS="-O1 -g -Wall -Wno-unused-function -Iruntime -D_FILE_OFFSET_BITS=64 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_CRT_RAND_S -D_POSIX_THREAD_SAFE_FUNCTIONS"
clang $FLAGS tests/test_ryuldn.c runtime/net/ryuldn.c -lws2_32 -pthread -o obj/ryuldn-test/test_ryuldn.exe
PORT=${PORT:-30499}
python tests/ryuldn_stub_server.py $PORT > obj/ryuldn-test/server.log 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT
sleep 1
obj/ryuldn-test/test_ryuldn.exe host 127.0.0.1:$PORT > obj/ryuldn-test/host.log 2>&1 &
HOST=$!
sleep 2
obj/ryuldn-test/test_ryuldn.exe join 127.0.0.1:$PORT > obj/ryuldn-test/join.log 2>&1 || { echo JOIN FAILED; cat obj/ryuldn-test/join.log; }
wait $HOST || { echo HOST FAILED; }
echo "--- host"; cat obj/ryuldn-test/host.log; echo "--- join"; cat obj/ryuldn-test/join.log; echo "--- server"; cat obj/ryuldn-test/server.log
