// The embedded Ryujinx LDN server's log lines into the port's log.
#include "../hle.h"
#include "ryusrv.h"

void ryusrv_log(const char *line) { LOG("[ryusrv] %s\n", line); }
