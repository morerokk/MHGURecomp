#!/bin/sh
# The Ryujinx LDN server the game runs itself (runtime/net/ryusrv/): the port's own client (tests/test_ryuldn.c) hosts and joins
# on it, then the mixed hub test runs with it as the server. No game, no separate LdnServer. Needs MSYS2 UCRT64 (clang).
set -u
cd "$(dirname "$0")/.."
mkdir -p obj/ryusrv-test
O=obj/ryusrv-test
R=runtime/net/ryusrv
FLAGS="-O1 -g -Wall -Wno-unused-function -Iruntime -D_FILE_OFFSET_BITS=64 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_CRT_RAND_S -D_POSIX_THREAD_SAFE_FUNCTIONS"
clang++ -std=c++20 -O1 -g -Wall -I$R/shim -I$R -D_WIN32_WINNT=0x0A00 tests/ryusrv_main.cpp $R/server.cpp $R/protocol.cpp $R/socket_util.cpp $R/ryusrv_c.cpp -lws2_32 -pthread -o $O/ryusrv_main.exe || exit 1
clang $FLAGS tests/test_ryuldn.c runtime/net/ryuldn.c -lws2_32 -pthread -o $O/test_ryuldn.exe || exit 1
PORT=${PORT:-30497}
$O/ryusrv_main.exe $PORT 150 > $O/server.log 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT
sleep 1
fail=0
$O/test_ryuldn.exe host 127.0.0.1:$PORT > $O/host.log 2>&1 &
H=$!
sleep 2
$O/test_ryuldn.exe join 127.0.0.1:$PORT > $O/join.log 2>&1 || { echo "JOIN FAILED"; fail=1; }
wait $H || { echo "HOST FAILED"; fail=1; }
echo "--- join"; cat $O/join.log; echo "--- server"; cat $O/server.log
echo "== mixed hub on the embedded server"
LDN_SERVER=127.0.0.1:$PORT sh tests/run_xbridge_test.sh 2>&1 | grep -E "FAIL|ALL PASSED|SOMETHING" || fail=1
[ $fail = 0 ] && echo "RYUSRV: ALL PASSED" || echo "RYUSRV: FAILED"
exit $fail
