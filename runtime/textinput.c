// Typed text for the game's text entries. On the Switch a text entry brings up the system keyboard (on screen, or a
// USB keyboard). Here, while the game asks for text, the physical keyboard types: characters go into the entry (the
// game shows them in its own text box as they change), Backspace deletes, Ctrl+V pastes, Enter confirms, Escape
// cancels. Meanwhile the keys don't act as controller buttons (L = A, WASD...): runtime/host_input.c asks
// textinput_event first. A controller can still confirm the entry as it is with Plus (Start), or cancel with B.
#include <pthread.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "overlay.h"
#include "rt.h"
#include "textinput.h"

#define TEXT_MAX 0x7d0  // bytes (the Switch keyboard's own limit is 500 characters)

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_active, g_window_on;
static char g_text[TEXT_MAX + 4];
static int g_max_chars, g_event;  // g_event: TI_* waiting for the game's next poll (ENTER / CANCEL win over CHANGED)

static int utf8_chars(const char *s) {
    int n = 0;
    for (; *s; s++) n += ((unsigned char)*s & 0xC0) != 0x80;
    return n;
}

static void post(int ev) {
    if (ev > g_event) g_event = ev;
}

void textinput_begin(const char *initial, int max_chars) {
    pthread_mutex_lock(&g_lock);
    g_active = true;
    g_max_chars = max_chars;
    g_event = TI_NONE;
    snprintf(g_text, sizeof g_text, "%s", initial ? initial : "");
    pthread_mutex_unlock(&g_lock);
    LOG("[textinput] text entry (up to %d characters, \"%s\"): type, Enter confirms, Escape cancels\n", max_chars,
        initial ? initial : "");
}

void textinput_end(void) {
    pthread_mutex_lock(&g_lock);
    g_active = false;
    g_event = TI_NONE;
    pthread_mutex_unlock(&g_lock);
}

int textinput_poll(char *out, size_t size, int *chars) {
    pthread_mutex_lock(&g_lock);
    int ev = g_event;
    g_event = TI_NONE;
    snprintf(out, size, "%s", g_text);
    if (chars) *chars = utf8_chars(g_text);
    pthread_mutex_unlock(&g_lock);
    return ev;
}

void textinput_peek(char *out, size_t size) {
    pthread_mutex_lock(&g_lock);
    snprintf(out, size, "%s", g_text);
    pthread_mutex_unlock(&g_lock);
}

bool textinput_active(void) { return __atomic_load_n(&g_active, __ATOMIC_RELAXED); }

void textinput_type(const char *s) {
    pthread_mutex_lock(&g_lock);
    if (g_active) {
        size_t len = strlen(g_text);
        int chars = utf8_chars(g_text);
        bool added = false;
        while (*s) {
            size_t n = 1;  // one character (its UTF-8 bytes)
            while (s[n] && ((unsigned char)s[n] & 0xC0) == 0x80) n++;
            bool control = (unsigned char)*s < 0x20 || *s == 0x7f;  // (pasted line breaks, tabs)
            if (!control) {
                if ((g_max_chars > 0 && chars >= g_max_chars) || len + n > TEXT_MAX) break;
                memcpy(g_text + len, s, n);
                g_text[len += n] = 0;
                chars++;
                added = true;
            }
            s += n;
        }
        if (added) post(TI_CHANGED);
    }
    pthread_mutex_unlock(&g_lock);
}

void textinput_key(int what) {
    pthread_mutex_lock(&g_lock);
    if (g_active) {
        if (what == TI_NONE) {  // Backspace: the last character
            size_t len = strlen(g_text);
            if (len) {
                while (len > 0 && ((unsigned char)g_text[len - 1] & 0xC0) == 0x80) len--;
                if (len > 0) len--;
                g_text[len] = 0;
                post(TI_CHANGED);
            }
        } else {
            post(what);
        }
    }
    pthread_mutex_unlock(&g_lock);
}

bool textinput_event(const SDL_Event *e) {
    if (!textinput_active()) return false;
    switch (e->type) {
    case SDL_EVENT_TEXT_INPUT:
        textinput_type(e->text.text);
        return true;
    case SDL_EVENT_KEY_DOWN: {
        SDL_Scancode sc = e->key.scancode;
        if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return false;  // (screenshots, MSAA...)
        if ((sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) && (e->key.mod & SDL_KMOD_ALT)) return false;
        if ((sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) && !e->key.repeat) textinput_key(TI_ENTER);
        else if (sc == SDL_SCANCODE_ESCAPE && !e->key.repeat) textinput_key(TI_CANCEL);
        else if (sc == SDL_SCANCODE_BACKSPACE) textinput_key(TI_NONE);
        else if (sc == SDL_SCANCODE_V && (e->key.mod & SDL_KMOD_CTRL)) {
            char *clip = SDL_GetClipboardText();
            if (clip) textinput_type(clip);
            SDL_free(clip);
        }
        return true;
    }
    case SDL_EVENT_KEY_UP:
        return true;
    }
    return false;
}

void textinput_update(void) {
    bool want = textinput_active() || overlay_menu_open();  // (Forge's menu takes typed text too)
    SDL_Window *w = ui_window();
    if (!w) return;
#ifdef __ANDROID__
    // The on-screen keyboard covers half the screen: it comes up only for a real text box (the game's entries, the chat,
    // a Forge plugin's field). The settings menu has its own on-screen keys, and a physical keyboard still types in it.
    static bool screen_keyboard = true;
    bool screen = textinput_active() || overlay_wants_text();
    if (screen != screen_keyboard) {
        if (g_window_on) SDL_StopTextInput(w), g_window_on = false;  // (before the hint changes: a shown keyboard hides)
        SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, screen ? "auto" : "0");
        screen_keyboard = screen;
    }
#endif
    if (want == g_window_on) return;
    if (want) SDL_StartTextInput(w);
    else SDL_StopTextInput(w);
    g_window_on = want;
}
