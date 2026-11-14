// The app's activity: SDL's own, which also keeps the status and navigation bars hidden. SDL asks for that with the
// system UI flags it set before Android 11, which newer Android versions (the app targets 15) don't always follow, so
// the bars stayed over the game on some devices. A swipe from the edge still shows them for a moment (and they hide
// again by themselves).
// It also asks to share the log after a crash (LogActivity), before the game starts and writes a new one, and shows
// SDL_ShowMessageBox's boxes as plain AlertDialogs (messageboxCreateAndShow).
package io.github.morerokk.mhgurecomp;

import android.app.AlertDialog;
import android.content.DialogInterface;
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.KeyEvent;
import android.view.View;
import android.view.Window;
import android.widget.Button;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

import org.libsdl.app.SDLActivity;

public class MHGUActivity extends SDLActivity {
    @Override
    protected void onCreate(Bundle saved) {
        if (saved == null) offerCrashLog();  // (before SDL starts the game, which starts log.txt over)
        super.onCreate(saved);
        hideBars();
        hideBarsSoon();
    }

    // The last run stopped on an error (runtime/platform/android.c left the marker): keep its log as crash-log.txt and
    // ask whether to share it. (A crash that comes back right away leaves a new marker for the next start.)
    private void offerCrashLog() {
        File dir = FilesFolder.get(this);  // (where runtime/platform/android.c writes them)
        if (dir == null) return;
        File marker = new File(dir, "last-run-crashed");
        if (!marker.exists()) return;
        marker.delete();
        File log = new File(dir, LogProvider.LOG), kept = new File(dir, LogProvider.CRASH_LOG);
        if (!log.isFile()) return;
        try (InputStream in = new FileInputStream(log); OutputStream out = new FileOutputStream(kept)) {
            byte[] buf = new byte[65536];
            for (int n; (n = in.read(buf)) > 0;) out.write(buf, 0, n);
        } catch (IOException e) {
            return;
        }
        startActivity(new Intent(this, LogActivity.class).putExtra(LogActivity.CRASH, true));
    }

    // SDL's own box puts the text and the buttons in a plain layout, so a text taller than the screen (a phone held
    // sideways, a large font) pushed the buttons off it and the box couldn't be answered; Back didn't answer it either.
    // An AlertDialog's message scrolls and its buttons stay on screen. SDL's thread waits on messageboxSelection until
    // the dialog is dismissed. The last of SDL's buttons (the main choice in the app's boxes) goes in the positive slot,
    // the one before it in the negative one, a first of three in the neutral one; where they sit is up to the theme
    // (this one puts the positive first). Back, Escape and a controller's B press the escape button, Enter the return
    // one when no button has the focus.
    @Override
    protected void messageboxCreateAndShow(Bundle args) {
        final int[] flags = args.getIntArray("buttonFlags"), ids = args.getIntArray("buttonIds");
        final String[] texts = args.getStringArray("buttonTexts");
        if (texts == null || flags == null || ids == null || texts.length == 0 || texts.length > 3) {
            super.messageboxCreateAndShow(args);  // (AlertDialog has three button slots; the app never asks more)
            return;
        }
        final AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle(args.getString("title"))
                .setMessage(args.getString("message"))
                .create();
        dialog.setCancelable(false);
        dialog.setOnDismissListener(new DialogInterface.OnDismissListener() {
            @Override
            public void onDismiss(DialogInterface unused) {
                synchronized (messageboxSelection) {
                    messageboxSelection.notify();
                }
            }
        });
        final int[] slots = {DialogInterface.BUTTON_POSITIVE, DialogInterface.BUTTON_NEGATIVE, DialogInterface.BUTTON_NEUTRAL};
        int escape = -1, enter = -1;
        for (int i = 0; i < texts.length; i++) {
            final int id = ids[i];
            dialog.setButton(slots[texts.length - 1 - i], texts[i], new DialogInterface.OnClickListener() {
                @Override
                public void onClick(DialogInterface d, int which) {
                    messageboxSelection[0] = id;  // (AlertDialog dismisses itself after this)
                }
            });
            if ((flags[i] & 1) != 0) enter = id;   // SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT
            if ((flags[i] & 2) != 0) escape = id;  // SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT
        }
        final int escapeId = escape, enterId = enter;
        dialog.setOnKeyListener(new DialogInterface.OnKeyListener() {
            @Override
            public boolean onKey(DialogInterface d, int keyCode, KeyEvent event) {
                int id;
                if (keyCode == KeyEvent.KEYCODE_BACK || keyCode == KeyEvent.KEYCODE_ESCAPE || keyCode == KeyEvent.KEYCODE_BUTTON_B) id = escapeId;
                else if (keyCode == KeyEvent.KEYCODE_ENTER || keyCode == KeyEvent.KEYCODE_NUMPAD_ENTER) {
                    View focus = dialog.getCurrentFocus();
                    if (focus instanceof Button) return false;  // (the focused button takes it)
                    id = enterId;
                } else return false;
                if (id < 0 && keyCode != KeyEvent.KEYCODE_BACK) return false;
                if (id >= 0 && event.getAction() == KeyEvent.ACTION_UP) {
                    messageboxSelection[0] = id;
                    dialog.dismiss();
                }
                return true;  // (Back with no escape button: nothing, the box stays)
            }
        });
        dialog.show();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) { hideBars(); hideBarsSoon(); }  // (back from a picker, the notification shade, the keyboard...)
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideBars();
    }

    // Some devices (handhelds with their own Android builds) show the bar again a moment after the window appears, after the
    // first hide: SDL sets its own flags when it creates the window. So hide once more shortly after the start and each time the
    // window comes back.
    private final Handler later = new Handler(Looper.getMainLooper());
    private final Runnable again = new Runnable() {
        @Override
        public void run() {
            hideBars();
        }
    };

    private void hideBarsSoon() {
        later.removeCallbacks(again);
        later.postDelayed(again, 300);
        later.postDelayed(again, 1000);
        later.postDelayed(again, 3000);
    }

    private void hideBars() {
        Window w = getWindow();
        if (w == null) return;
        if (Build.VERSION.SDK_INT >= 30) {
            w.setDecorFitsSystemWindows(false);
            WindowInsetsController c = w.getInsetsController();
            if (c != null) {
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                c.hide(WindowInsets.Type.systemBars());
            }
            w.getAttributes().layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
        }
    }
}
