// Mouse look on the hunt camera's own angles (runtime/hle/mousecam.c)
#pragma once
#include <stdbool.h>

void mousecam_init(void);              // hooks the quest camera's stick handler
bool mousecam_active(void);            // the hunt camera takes the mouse now (its stick handler ran just now)
void mousecam_add(float dx, float dy);  // counts the mouse moved (x right, y down; any thread)
