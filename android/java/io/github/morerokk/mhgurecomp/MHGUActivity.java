// The app's activity: SDL's own, which also keeps the status and navigation bars hidden. SDL asks for that with the
// system UI flags it set before Android 11, which newer Android versions (the app targets 15) don't always follow, so
// the bars stayed over the game on some devices. A swipe from the edge still shows them for a moment (and they hide
// again by themselves).
// It also asks to share the log after a crash (LogActivity), before the game starts and writes a new one.
package io.github.morerokk.mhgurecomp;

import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Window;
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
        File dir = getExternalFilesDir(null);
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
