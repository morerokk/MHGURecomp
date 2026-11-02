// The app's activity: SDL's own, which also keeps the status and navigation bars hidden. SDL asks for that with the
// system UI flags it set before Android 11, which newer Android versions (the app targets 15) don't always follow, so
// the bars stayed over the game on some devices. A swipe from the edge still shows them for a moment (and they hide
// again by themselves).
package io.github.morerokk.mhgurecomp;

import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

public class MHGUActivity extends SDLActivity {
    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        hideBars();
        hideBarsSoon();
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
