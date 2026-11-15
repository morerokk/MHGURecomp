// The hunt camera stuck in a fixed view after a cut-in at high frame rates. Reported: a monster breaking the ledge in
// the Ancestral Steppe's area 7 (stage 11) takes the camera to show it, and the camera then stays looking down at the
// area for the rest of the hunt; on emulators too, only above 30 fps.
// The common script's camera helper 0x5d6df0 (r1: its state, s16: 0 idle, 10 showing, 100 done; r3: the script's
// view; [fp + 8] end now) runs every update while a cut-in is set up: when the script says "show" (0x5d17f8 answers
// 1) it pushes the view on the stage camera's stack (0x21b08c -> 0x2162a8, count at stage camera +0x39c) and sets
// state 10; when the script stops saying so, it pops one view (0x21b794 -> 0x21635c) and sets 100. It doesn't check
// that its view is already up, so each update the script says "show" pushes another view. At 30 fps that is one
// update (one push, one pop); at 240 fps the same time is 4 updates: 4 pushes, 1 pop, and the 3 views left keep the
// camera on the cut-in's view for good (the quest camera drives itself only with an empty stack).
// Fix (tools/gen.py PATCHES, 0x5d6e74, the first instruction of the push): in state 10 the push is skipped and the
// helper goes on to its end (0x5d6fc4), as when the script asks for nothing new, so the view is pushed once. At 30 fps
// the same as the original. MHGU_NO_STAGEVIEW=1: the original (for comparing).
// Found with MHGU_CAMTRACE (camtrace.c): quest 924 started in area 7, the hunter in the ledge's sub-area 1, the
// ledge (area 7 id 1, type 6) collapsed with its command 0x18.
#include <stdlib.h>

#include "../hle.h"

int stageview_skip_push(uint32_t state) {
    static int off = -1;
    if (off < 0) {
        const char *e = getenv("MHGU_NO_STAGEVIEW");
        off = e && *e == '1';
    }
    return !off && LD16(state) == 10;
}
