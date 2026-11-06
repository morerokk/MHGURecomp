// Stand-in for the suyu tree's logging macros: the lines go to the port's log (ryusrv_log in ryusrv_c.c).
#pragma once
#include <string>
#include "fmt/format.h"
extern "C" void ryusrv_log(const char* line);
#define LOG_INFO(cls, ...) ryusrv_log(fmt::format(__VA_ARGS__).c_str())
#define LOG_WARNING(cls, ...) ryusrv_log(fmt::format(__VA_ARGS__).c_str())
#define LOG_ERROR(cls, ...) ryusrv_log(fmt::format(__VA_ARGS__).c_str())
