// Typed chat anywhere. The game only lets you type in the Hub (Minus opens the keyboard; in a quest Minus is the kick),
// but a shoutout in a quest is sent as its text: one netChatTextData (0xac bytes: u8 type at +9, the sender's name
// at +0xa, UTF-8 text at +0x32), the same message as Hub chat (type 1 + the sender's player index; a shoutout 6 +
// index), shown in every player's chat window as it arrives. So "/" opens a line to type (runtime/textinput.c, drawn
// by the overlay) and Enter sends it the way the game sends a shoutout, from the game's thread. The window doesn't wrap
// (a long row runs out of it), but whoever receives a message sets each line of it as a row of its own, so a longer
// line goes out with line breaks in place of the spaces where it should wrap: one message, as long as the text fits.
#include "chat.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "../config.h"
#include "../hle.h"
#include "../textinput.h"

// Game code and data (main, v1.4.0; runtime addresses). Found from the Hub chat's own sender (0x870270).
#define FN_SEND_CHAT   0x41e204u  // (net object, type, name, text[0x78], int on the stack (1)): builds and sends it
#define FN_NET_INDEX   0x427ae4u  // (sNetwork) -> our player index in the session
#define FN_NET_ONLINE  0x427ba8u  // (sNetwork) -> in a session (else the index is 0)
#define FN_NET_OBJECT  0x427b3cu  // (sNetwork, 0x6b) -> the object chat messages are sent through
#define FN_MY_PLAYER   0x476278u  // (sPlayer) -> our player; +0x13c4 -> its data, +0x514 there: the hunter's name
#define G_SNETWORK     0x1a843f0u
#define G_SPLAYER      0x1a85b58u
#define CHAT_TYPE      6u         // + player index: a shoutout's type (shown in quests too)
#define TEXT_BYTES     0x78u      // what the sender copies, and the receiver into a buffer of its own (0x41e580)
#define MAX_CHARS      120        // a line typed; it wraps between words, a message holding as many rows as fit
#define ROW_WIDTH      40         // what a row of the chat window shows, in Latin letters (see next_piece)
#define MESSAGE_ROWS   5          // the receiver sets a row and calls itself for the rest, at most 5 deep (0x41e580)
#define FN_RECEIVE     0x41e580u  // (net object, message, sender's index): sets a message's rows in the chat window
#define G_RECEIVE_DEPTH 0x1a64300u // s8: how deep that is in its own calls (above 0: a later row of a message)
#define MSG_TYPE       9u
#define MSG_NAME       0xau

static atomic_bool g_composing;
static float g_left = .016f, g_bottom = .74f, g_box[4];  // (above the game's chat window, bottom left)
static bool g_loaded;
#define POSITION_FILE "chat_box.txt"  // (beside mhgurecomp.ini)

bool chat_active(void) { return atomic_load(&g_composing); }

void chat_position(float *left, float *bottom) {
    if (!g_loaded) {
        g_loaded = true;
        FILE *f = fopen(POSITION_FILE, "r");
        float x, y;
        if (f && fscanf(f, "%f %f", &x, &y) == 2 && x >= 0 && x <= 1 && y >= 0 && y <= 1) g_left = x, g_bottom = y;
        if (f) fclose(f);
    }
    *left = g_left, *bottom = g_bottom;
}

void chat_set_box(float x0, float y0, float x1, float y1) { g_box[0] = x0, g_box[1] = y0, g_box[2] = x1, g_box[3] = y1; }

bool chat_in_box(float x, float y) { return x >= g_box[0] && x <= g_box[2] && y >= g_box[1] && y <= g_box[3]; }

void chat_move(float dx, float dy) {
    float w = g_box[2] - g_box[0], h = g_box[3] - g_box[1];
    g_left += dx, g_bottom += dy;
    if (g_left > 1 - w) g_left = 1 - w;  // (kept on the screen)
    if (g_left < 0) g_left = 0;
    if (g_bottom > 1) g_bottom = 1;
    if (g_bottom < h) g_bottom = h;
}

void chat_save_position(void) {
    FILE *f = fopen(POSITION_FILE, "w");
    if (!f) return;
    fprintf(f, "%.4f %.4f\n", g_left, g_bottom);
    fclose(f);
}

void chat_open(void) {
    if (atomic_load(&g_composing)) return;
    textinput_begin("", MAX_CHARS);
    atomic_store(&g_composing, true);  // (after it: chat_tick never sees the line without its text entry, below)
}

void chat_close(void) { atomic_store(&g_composing, false); }

bool chat_composing(char *text, size_t size) {
    if (!atomic_load(&g_composing)) return false;
    textinput_peek(text, size);
    return true;
}

static uint32_t call(Cpu *c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    c->r[0] = a0, c->r[1] = a1, c->r[2] = a2, c->r[3] = a3;
    guest_call(c, fn);
    return c->r[0];
}

static void send_text(Cpu *c, const char *text) {
    uint32_t net = LD32(G_SNETWORK), players = LD32(G_SPLAYER);
    if (!net || !players) { LOG("[chat] not sent: no network or player yet\n"); return; }
    uint32_t saved[16];
    memcpy(saved, c->r, sizeof saved);
    int32_t index = (int32_t)call(c, FN_NET_INDEX, net, 0, 0, 0);
    if (!call(c, FN_NET_ONLINE, net, 0, 0, 0)) index = 0;
    uint32_t object = index < 0 ? 0 : call(c, FN_NET_OBJECT, net, 0x6b, 0, 0);
    uint32_t me = call(c, FN_MY_PLAYER, players, 0, 0, 0), data = me ? LD32(me + 0x13c4) : 0;
    if (!object || !data) {
        LOG("[chat] not sent: %s\n", !object ? "no chat connection" : "no hunter");
    } else {
        uint32_t sp = (saved[13] - 0x100) & ~7u, buf = sp + 0x10;  // (the text and the stack argument, below the frame)
        memset(G2H(buf), 0, TEXT_BYTES);
        memcpy(G2H(buf), text, strlen(text));
        ST32(sp, 1);
        c->r[13] = sp;
        call(c, FN_SEND_CHAT, object, CHAT_TYPE + (uint32_t)index, data + 0x514, buf);
        LOG("[chat] sent \"%s\"\n", text);
    }
    memcpy(c->r, saved, sizeof saved);
}

static int utf8_len(const char *s) { return (*s & 0x80) == 0 ? 1 : (*s & 0xE0) == 0xC0 ? 2 : (*s & 0xF0) == 0xE0 ? 3 : 4; }

// The next row of a line: up to ROW_WIDTH wide (kana and kanji, 3 bytes or more, count as two letters), ending before
// a space if there is one (a word longer than that is cut); `*rest` is where the next one starts (spaces skipped)
static size_t next_piece(const char *s, const char **rest) {
    size_t n = 0, cut = 0;
    int width = 0;
    while (s[n]) {
        int k = utf8_len(s + n), w = k >= 3 ? 2 : 1;
        if (width + w > ROW_WIDTH) break;
        if (s[n] == ' ') cut = n;
        n += (size_t)k, width += w;
    }
    if (s[n] && s[n] != ' ' && cut) n = cut;  // (the line goes on in the middle of a word: end before it)
    const char *r = s + n;
    while (*r == ' ') r++;
    *rest = r;
    while (n && s[n - 1] == ' ') n--;
    return n;
}

// The sender's name only on a message's first row: the rows after it read as the same line going on. (Only a typed
// line's type: the game's own notices keep theirs. The message goes back as it came.)
static void receive_message(Cpu *c) {
    uint32_t name = c->r[1] + MSG_NAME;
    uint32_t type = LD8(c->r[1] + MSG_TYPE);
    uint8_t first = LD8(name);
    bool later_row = (int8_t)LD8(G_RECEIVE_DEPTH) > 0 && type >= CHAT_TYPE && type < CHAT_TYPE + 4;
    if (later_row) ST8(name, 0);
    fnhook_call_original(c, FN_RECEIVE);
    if (later_row) ST8(name, first);
}

// ---- The chat key in the bottom right button guide (issue #399: players didn't know the key, nor that "/" names a key
// position, the US layout's). In a town the guide shows the game's ZR "Chat" (Shoutouts, the chat log, the game's own
// Keyboard: mostly for reading what was just posted), so that one is called "Chat Log" and the chat key gets "Chat",
// both the game's own words in every language (buttonName 95 and 142). Its icon is always the key's cap, with the
// controller too: chat has no button.
//
// The guide (cUIOUnderKeyAssign, runtime/hle/villagehub.c has more) takes each prompt from its preset, a slot's own
// {buttonName icon, buttonName label} (+0x248, 8 bytes a slot: a town sets slot 2 to ZR "Chat" through 0x844d08, which
// is hooked to give it "Chat Log"), or up to 4 extras {buttonName icon, label text *} (+0x228; the extra's adder
// 0x844c34 is hooked too, as one way of adding the game's Chat, 0x99d3a4, goes there). An extra draws the text of its
// icon's buttonName entry, then the label: the chat key's extra has an index past buttonName's end (the game gives
// "" for it) and the label "<ICON CHATKEY>Chat". It is put in (or taken out) as the guide is drawn (0x8439ec, hooked):
// the town's code sets the guide's state again as it goes, and an extra added between two drawings didn't show. The
// text's icon names come from one object (*0x232668c, its vtable +0x48: (this, name, the fontIcon table) -> a button
// font code); its hook gives CHATKEY the code of glyph 'o', a cell of the button font no name gives (so no text of the
// game draws it), and runtime/hle/prompts.c draws the key's cap in that cell while the prompt shows (chat_key_prompt).
// Shown in the Hub and the Pub while in a session, where the game shows its ZR prompt; not while a line is typed.
#define FN_GUIDE_GET     0x85e758u  // () -> the bottom right button guide or 0
#define FN_GUIDE_SHOW    0x8439ecu  // (guide, preset, flags, redraw): draws it (only on a change, unless redraw is 1)
#define FN_GUIDE_SLOT    0x844d08u  // (guide, slot, icon, label): a slot's own prompt
#define FN_GUIDE_EXTRA   0x844c34u  // (guide, icon, label *): an extra prompt (no redraw)
#define GUIDE_PRESET     0x4cu
#define GUIDE_FLAGS      0x50u
#define GUIDE_EXTRA      0x228u     // 4 x {u32 icon (-1 empty), u32 label}
#define GUIDE_SLOT       0x248u     // 10 x {u32 icon (-1: the preset's), u16 label}
#define G_MESSAGES       0x1a99220u // the message manager *: +0x5c -> -> its tables, [0x12] buttonName
#define GMD_BUTTON_NAME  0x12u
#define GMD_COUNT        0x88u      // a message table: its count, and its strings (u32 * each)
#define GMD_STRINGS      0x98u
#define BN_CHAT          142u
#define BN_CHAT_LOG      95u
#define BN_ICON_ZR       0x81u      // (the town's Chat: ZR, or R with the ZR / R swap)
#define BN_ICON_R        5u
#define BN_NONE          999u       // (past buttonName's end: "")
#define G_ICON_NAMES     0x232668cu // the text's icon names (see above)
#define ICON_NAME        "CHATKEY"
#define ICON_CODE        0x6fu
#define G_GAMECONTROL    0x1a8ca48u // sGameControl *: +0x4c the area's type (runtime/hle/villagehub.c)
#define GC_AREA_TYPE     0x4cu
#define AREA_HUB         0
#define AREA_PUB         7

static atomic_int g_key_prompt;  // the chat key's scancode while its prompt is in the guide, else 0 (prompts.c)
static atomic_int g_key_wanted;  // the chat key while its prompt should show where the game shows its Chat, else 0
static uint32_t g_icon_fn;       // the icon name lookup, once hooked
static uint32_t g_label;         // guest: "<ICON CHATKEY>" and buttonName's Chat
static uint32_t g_label_from;    // (the Chat it was made from: another language's makes it again)

int chat_key_prompt(void) { return atomic_load(&g_key_prompt); }

// buttonName's text i (as 0x75bc4c / 0xd84114 read it), 0 when the table isn't there
static uint32_t button_name(uint32_t i) {
    uint32_t m = LD32(G_MESSAGES), t = m ? LD32(m + 0x5c) : 0;
    t = t ? LD32(t) : 0;
    t = t ? LD32(t) : 0;
    uint32_t g = t ? LD32(t + 4 * GMD_BUTTON_NAME) : 0;
    if (!g || i >= LD32(g + GMD_COUNT)) return 0;
    uint32_t s = LD32(g + GMD_STRINGS);
    return s ? LD32(s + 4 * i) : 0;
}

static void guide_slot(Cpu *c) {
    if ((c->r[3] & 0xffff) == BN_CHAT) c->r[3] = (c->r[3] & ~0xffffu) | BN_CHAT_LOG;
    fnhook_call_original(c, FN_GUIDE_SLOT);
}

static void guide_extra(Cpu *c) {
    uint32_t chat = button_name(BN_CHAT), log = button_name(BN_CHAT_LOG);
    if (chat && log && c->r[2] == chat) c->r[2] = log;
    fnhook_call_original(c, FN_GUIDE_EXTRA);
}

static void icon_code(Cpu *c) {
    if (c->r[1] && !strcmp((const char *)G2H(c->r[1]), ICON_NAME)) {
        c->r[0] = ICON_CODE;
        return;
    }
    fnhook_call_original(c, g_icon_fn);
}

static void guide_show(Cpu *c);

void chat_init(void) {
    fnhook_set(FN_RECEIVE, hle_register("chat:receive", receive_message));
    fnhook_set(FN_GUIDE_SLOT, hle_register("chat:guide_slot", guide_slot));
    fnhook_set(FN_GUIDE_EXTRA, hle_register("chat:guide_extra", guide_extra));
    fnhook_set(FN_GUIDE_SHOW, hle_register("chat:guide_show", guide_show));
    g_label = hle_alloc(160);
}

// (registers kept: chat_tick runs inside the game's input call)
static uint32_t call_kept(Cpu *c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    uint32_t saved[16];
    memcpy(saved, c->r, sizeof saved);
    uint32_t r = call(c, fn, a0, a1, a2, a3);
    memcpy(c->r, saved, sizeof saved);
    return r;
}

// The chat key's extra in guide kg as it should be now, before the guide is drawn; true when that changed it
static bool place_key_prompt(uint32_t kg, bool fresh) {
    bool town_chat = false;  // (the game's ZR Chat, renamed, is in the guide: a town as you walk around)
    for (uint32_t i = 0; i < 10 && !town_chat; i++) {
        uint32_t s = kg + GUIDE_SLOT + 8 * i, icon = LD32(s), label = LD16(s + 4);
        town_chat = (icon == BN_ICON_ZR || icon == BN_ICON_R) && (label == BN_CHAT_LOG || label == BN_CHAT);
    }
    int key = atomic_load(&g_key_wanted);
    bool want = key && town_chat, have = false, changed = false;
    uint32_t room = 0;
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t e = kg + GUIDE_EXTRA + 8 * i;
        if (LD32(e) == 0xffffffffu) { if (!room) room = e; continue; }
        if (LD32(e + 4) != g_label) continue;
        if (want && !have && !fresh) { have = true; continue; }
        ST32(e, 0xffffffffu), ST32(e + 4, 0);  // (not wanted, a second one, or its text changed: added again below)
        if (!room) room = e;
        changed = true;
    }
    if (want && !have && room) ST32(room, BN_NONE), ST32(room + 4, g_label), have = changed = true;
    atomic_store(&g_key_prompt, have ? key : 0);
    return changed;
}

// Every drawing of the guide gets the extra as it should be (a town's code sets the guide's state again as it goes:
// an extra added between two drawings didn't last)
static bool g_label_new;  // (the label's text changed: added again)
static void guide_show(Cpu *c) {
    uint32_t kg = c->r[0];
    if (kg && g_label && place_key_prompt(kg, g_label_new)) c->r[3] = 1;  // (drawn again, though preset and flags are the same)
    g_label_new = false;
    fnhook_call_original(c, FN_GUIDE_SHOW);
}

static void key_prompt_update(Cpu *c) {
    static uint32_t frames;
    static bool online_town;
    if (!g_icon_fn) {  // (the object is made at start-up)
        uint32_t o = LD32(G_ICON_NAMES), vt = o ? LD32(o) : 0, fn = vt ? LD32(vt + 0x48) : 0;
        if (fn && fnhook_set(fn, hle_register("chat:icon_code", icon_code))) g_icon_fn = fn;
        else if (fn) g_icon_fn = 1;  // (not hookable: no prompt)
    }
    if (frames++ % 30 == 0) {  // (twice a second or so: in the Hub or the Pub, in a session)
        uint32_t gc = LD32(G_GAMECONTROL), net = LD32(G_SNETWORK);
        uint8_t area = gc ? LD8(gc + GC_AREA_TYPE) : 0xff;
        online_town = (area == AREA_HUB || area == AREA_PUB) && net && call_kept(c, FN_NET_ONLINE, net, 0, 0, 0);
    }
    uint32_t chat = button_name(BN_CHAT);
    bool redraw = false;
    if (chat && chat != g_label_from) {  // (the label, in the game's language)
        snprintf((char *)G2H(g_label), 160, "<ICON %s>%.140s", ICON_NAME, (const char *)G2H(chat));
        redraw = g_label_new = g_label_from != 0;
        g_label_from = chat;
    }
    int key = config_live_chat_key();
    int want = g_icon_fn > 1 && g_label_from && online_town && !atomic_load(&g_composing) ? key : 0;
    if (want != atomic_exchange(&g_key_wanted, want)) redraw = true;
    uint32_t kg = redraw ? call_kept(c, FN_GUIDE_GET, 0, 0, 0, 0) : 0;
    if (kg) call_kept(c, FN_GUIDE_SHOW, kg, LD32(kg + GUIDE_PRESET), LD32(kg + GUIDE_FLAGS), 1);
}

void chat_tick(Cpu *c) {
    key_prompt_update(c);
    if (!atomic_load(&g_composing)) return;
    if (!textinput_active()) {  // (ended under it: the game's keyboard calls (swkbd.c Disappear, Finalize) end any entry;
                                // the box would stay, and the chat key no longer open one)
        atomic_store(&g_composing, false);
        return;
    }
    char text[512];
    int chars, ev = textinput_poll(text, sizeof text, &chars);
    if (ev != TI_ENTER && ev != TI_CANCEL) return;
    textinput_end();
    atomic_store(&g_composing, false);
    if (ev == TI_CANCEL) return;
    const char *s = text;
    while (*s == ' ') s++;
    while (*s) {
        char message[TEXT_BYTES];  // (TEXT_BYTES - 1 of text: the last stays 0)
        size_t len = 0;
        for (int rows = 0; *s && rows < MESSAGE_ROWS; rows++) {
            const char *rest;
            size_t n = next_piece(s, &rest);
            if (len + (len != 0) + n > sizeof message - 1) break;
            if (len) message[len++] = '\n';
            memcpy(message + len, s, n);
            len += n;
            s = rest;
        }
        if (!len) break;
        message[len] = 0;
        send_text(c, message);
    }
}
