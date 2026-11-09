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

void chat_init(void) { fnhook_set(FN_RECEIVE, hle_register("chat:receive", receive_message)); }

void chat_tick(Cpu *c) {
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
