// The app's activity: SDL's own, which also keeps the status and navigation bars hidden. SDL asks for that with the
// system UI flags it set before Android 11, which newer Android versions (the app targets 15) don't always follow, so
// the bars stayed over the game on some devices. A swipe from the edge still shows them for a moment (and they hide
// again by themselves).
package io.github.morerokk.mhgurecomp;

import android.os.Build;
import android.os.Bundle;
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
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideBars();  // (back from a picker, the notification shade, the keyboard...)
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
