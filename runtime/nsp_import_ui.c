// SDL file pickers + progress window for desktop NSP import (mirrors updater UX).
#include "nsp_import.h"

#include "platform.h"
#include "rt.h"

#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#ifndef __ANDROID__

enum { W = 520, H = 280 };

typedef struct {
    SDL_Mutex *lock;
    char stage[256];
    float frac;
    volatile int cancel;
    int done;  // 0 running, 1 ok, 2 err, 3 cancelled
    char error[512];
    NspImportRequest *req;
} UiState;

typedef struct {
    char *target;
    size_t size;
    SDL_AtomicInt ready;  // 1 ok, -1 cancel
} FilePick;

static void SDLCALL file_pick_cb(void *userdata, const char *const *files, int filter) {
    (void)filter;
    FilePick *p = userdata;
    if (files && files[0] && files[0][0]) {
        snprintf(p->target, p->size, "%s", files[0]);
        SDL_SetAtomicInt(&p->ready, 1);
    } else {
        SDL_SetAtomicInt(&p->ready, -1);
    }
}

static bool pick_file(SDL_Window *win, const char *title, const SDL_DialogFileFilter *filters, int nfilters,
                      char *out, size_t size) {
    FilePick pick = {out, size, {0}};
    out[0] = 0;
    SDL_ShowOpenFileDialog(file_pick_cb, &pick, win, filters, nfilters, NULL, false);
    while (SDL_GetAtomicInt(&pick.ready) == 0) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) {
                SDL_SetAtomicInt(&pick.ready, -1);
                break;
            }
        }
        SDL_Delay(16);
    }
    return SDL_GetAtomicInt(&pick.ready) > 0 && out[0];
}

static void on_progress(void *user, const char *stage, float frac) {
    UiState *st = user;
    SDL_LockMutex(st->lock);
    if (stage) snprintf(st->stage, sizeof st->stage, "%s", stage);
    st->frac = frac;
    SDL_UnlockMutex(st->lock);
}

static int SDLCALL import_worker(void *data) {
    UiState *st = data;
    int r = nsp_import_run(st->req);
    SDL_LockMutex(st->lock);
    if (r == NSP_IMPORT_OK)
        st->done = 1;
    else if (r == NSP_IMPORT_CANCELLED)
        st->done = 3;
    else {
        st->done = 2;
        snprintf(st->error, sizeof st->error, "%s", st->req->error);
    }
    SDL_UnlockMutex(st->lock);
    return 0;
}

static void draw_bar(SDL_Renderer *rd, float frac) {
    SDL_FRect bg = {40, 150, W - 80, 22};
    SDL_SetRenderDrawColor(rd, 40, 40, 48, 255);
    SDL_RenderFillRect(rd, &bg);
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    SDL_FRect fg = {bg.x, bg.y, bg.w * frac, bg.h};
    SDL_SetRenderDrawColor(rd, 70, 140, 220, 255);
    SDL_RenderFillRect(rd, &fg);
    SDL_SetRenderDrawColor(rd, 90, 90, 100, 255);
    SDL_RenderRect(rd, &bg);
}

static void draw_text(SDL_Renderer *rd, float x, float y, const char *text) {
    // SDL3 has no built-in font without TTF; use debug text API if available.
#if SDL_VERSION_ATLEAST(3, 2, 0)
    SDL_SetRenderScale(rd, 1.0f, 1.0f);
    SDL_SetRenderDrawColor(rd, 230, 230, 230, 255);
    SDL_RenderDebugText(rd, x, y, text);
#else
    (void)rd;
    (void)x;
    (void)y;
    (void)text;
#endif
}

static bool run_progress_ui(NspImportRequest *req) {
    UiState st = {0};
    st.lock = SDL_CreateMutex();
    st.req = req;
    req->cancel = &st.cancel;
    req->progress = on_progress;
    req->progress_user = &st;
    snprintf(st.stage, sizeof st.stage, "Starting…");

    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        int r = nsp_import_run(req);
        SDL_DestroyMutex(st.lock);
        if (r == NSP_IMPORT_OK) return true;
        if (r != NSP_IMPORT_CANCELLED && req->error[0]) plat_error_box(req->error);
        return false;
    }

    SDL_Window *win = NULL;
    SDL_Renderer *rd = NULL;
    if (!SDL_CreateWindowAndRenderer("MHGU Recomp — Import NSP", W, H, 0, &win, &rd)) {
        int r = nsp_import_run(req);
        SDL_DestroyMutex(st.lock);
        if (r == NSP_IMPORT_OK) return true;
        if (r != NSP_IMPORT_CANCELLED && req->error[0]) plat_error_box(req->error);
        return false;
    }
    SDL_SetRenderVSync(rd, 1);

    SDL_Thread *th = SDL_CreateThread(import_worker, "nsp_import", &st);
    if (!th) {
        SDL_DestroyRenderer(rd);
        SDL_DestroyWindow(win);
        SDL_DestroyMutex(st.lock);
        plat_error_box("Could not start the import worker.");
        return false;
    }

    bool quit = false;
    while (!quit) {
        int done;
        char stage[256];
        float frac;
        char err[512];
        SDL_LockMutex(st.lock);
        done = st.done;
        frac = st.frac;
        snprintf(stage, sizeof stage, "%s", st.stage);
        snprintf(err, sizeof err, "%s", st.error);
        SDL_UnlockMutex(st.lock);

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) st.cancel = 1;
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) st.cancel = 1;
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                float mx = e.button.x, my = e.button.y;
                if (mx >= W / 2 - 50 && mx <= W / 2 + 50 && my >= H - 55 && my <= H - 25) st.cancel = 1;
            }
        }

        SDL_SetRenderDrawColor(rd, 24, 24, 28, 255);
        SDL_RenderClear(rd);
        draw_text(rd, 40, 40, "Importing Monster Hunter Generations Ultimate");
        draw_text(rd, 40, 70, stage);
        draw_bar(rd, frac);
        char pct[32];
        snprintf(pct, sizeof pct, "%d%%", (int)(frac * 100 + 0.5f));
        draw_text(rd, 40, 180, pct);
        // Cancel button
        SDL_FRect btn = {W / 2.0f - 50, H - 55, 100, 30};
        SDL_SetRenderDrawColor(rd, 60, 60, 70, 255);
        SDL_RenderFillRect(rd, &btn);
        SDL_SetRenderDrawColor(rd, 120, 120, 130, 255);
        SDL_RenderRect(rd, &btn);
        draw_text(rd, btn.x + 28, btn.y + 8, done ? "Close" : "Cancel");
        SDL_RenderPresent(rd);

        if (done) {
            if (done == 2 && err[0]) {
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "MHGU Recomp", err, win);
            } else if (done == 3) {
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "MHGU Recomp", "Import cancelled.", win);
            } else if (done == 1) {
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "MHGU Recomp",
                                        "Game files installed. Starting…", win);
            }
            quit = true;
        }
        SDL_Delay(16);
    }

    SDL_WaitThread(th, NULL);
    SDL_DestroyRenderer(rd);
    SDL_DestroyWindow(win);
    int ok = 0;
    SDL_LockMutex(st.lock);
    ok = st.done == 1;
    SDL_UnlockMutex(st.lock);
    SDL_DestroyMutex(st.lock);
    return ok;
}

bool nsp_import_offer(const char *msg, char *data_dir, size_t data_size, bool confirm_overwrite) {
    (void)data_size;
    char text[2600];
    snprintf(text, sizeof text,
             "%s\n\nImport from your base game NSP and the 1.4.0 update NSP?\n"
             "You will need your Switch prod.keys. About 20 GB free space is required while extracting.\n"
             "Original NSP files are not modified.",
             msg ? msg : "Game files were not found.");

    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit"},
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Import from NSP…"},
    };
    const SDL_MessageBoxData box = {SDL_MESSAGEBOX_INFORMATION, NULL, "MHGU Recomp", text, 2, buttons, NULL};
    int pressed = 0;
    if (!SDL_ShowMessageBox(&box, &pressed) || pressed != 1) return false;

    if (confirm_overwrite) {
        const SDL_MessageBoxButtonData yn[] = {
            {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"},
            {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Replace"},
        };
        const SDL_MessageBoxData ask = {
            SDL_MESSAGEBOX_WARNING, NULL, "MHGU Recomp",
            "Valid game files are already in data/. Replace them with a new NSP import?", 2, yn, NULL};
        int a = 0;
        if (!SDL_ShowMessageBox(&ask, &a) || a != 1) return false;
    }

    SDL_InitSubSystem(SDL_INIT_VIDEO);
    SDL_Window *hidden = SDL_CreateWindow("MHGU Recomp", 1, 1, SDL_WINDOW_HIDDEN);

    char base_nsp[1200] = "", update_nsp[1200] = "", prod_keys[1200] = "", title_keys[1200] = "";
    static const SDL_DialogFileFilter nsp_filters[] = {{"Nintendo Submission Package", "nsp"}, {"All files", "*"}};
    static const SDL_DialogFileFilter keys_filters[] = {{"Switch keys", "keys"}, {"All files", "*"}};

    if (!pick_file(hidden, "Select base game NSP", nsp_filters, 2, base_nsp, sizeof base_nsp)) {
        if (hidden) SDL_DestroyWindow(hidden);
        return true;  // re-offer
    }
    if (!pick_file(hidden, "Select 1.4.0 update NSP", nsp_filters, 2, update_nsp, sizeof update_nsp)) {
        if (hidden) SDL_DestroyWindow(hidden);
        return true;
    }
    if (!pick_file(hidden, "Select prod.keys", keys_filters, 2, prod_keys, sizeof prod_keys)) {
        if (hidden) SDL_DestroyWindow(hidden);
        return true;
    }
    // Optional title.keys
    {
        const SDL_MessageBoxButtonData yn[] = {
            {0, 0, "Skip"},
            {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Select title.keys"},
        };
        const SDL_MessageBoxData ask = {SDL_MESSAGEBOX_INFORMATION, hidden, "MHGU Recomp",
                                       "Do you also have a title.keys file? (optional; tickets inside the NSP are used "
                                       "when possible)",
                                       2, yn, NULL};
        int a = 0;
        if (SDL_ShowMessageBox(&ask, &a) && a == 1)
            pick_file(hidden, "Select title.keys (optional)", keys_filters, 2, title_keys, sizeof title_keys);
    }
    if (hidden) SDL_DestroyWindow(hidden);

    NspImportRequest req = {0};
    req.data_dir = data_dir;
    req.base_nsp = base_nsp;
    req.update_nsp = update_nsp;
    req.prod_keys = prod_keys;
    req.title_keys = title_keys[0] ? title_keys : NULL;
    run_progress_ui(&req);
    return true;  // recheck game_files_problem / offer again on failure
}

#else  // __ANDROID__

bool nsp_import_offer(const char *msg, char *data_dir, size_t data_size, bool confirm_overwrite) {
    (void)msg;
    (void)data_dir;
    (void)data_size;
    (void)confirm_overwrite;
    return false;
}

#endif
