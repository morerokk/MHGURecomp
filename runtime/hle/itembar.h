// The mouse wheel on the item bar (runtime/hle/itembar.c)
#pragma once

void itembar_init(void);         // hooks the player's item bar handler
void itembar_scroll(int steps);  // wheel notches: < 0 the item to the left, > 0 to the right (any thread)
