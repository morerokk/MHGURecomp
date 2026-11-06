// Runs the embedded Ryujinx LDN server (runtime/net/ryusrv/) on its own for tests/run_ryusrv_test.sh: ryusrv_main <port> <seconds>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "../runtime/net/ryusrv.h"

extern "C" void ryusrv_log(const char *line) { std::printf("[ryusrv] %s\n", line); std::fflush(stdout); }

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    char err[96];
    if (!ryusrv_start(std::atoi(argv[1]), err, sizeof err)) { std::printf("can't start: %s\n", err); return 1; }
    std::this_thread::sleep_for(std::chrono::seconds(std::atoi(argv[2])));
    ryusrv_stop();
    return 0;
}
