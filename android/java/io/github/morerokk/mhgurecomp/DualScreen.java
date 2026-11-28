// The bottom screen of a dual-screen handheld ([Dual Screen] SecondDisplay; runtime/nvn/vulkan/second_display_android.c):
// a Presentation on a display other than the game's, holding a SurfaceView. Its Surface goes to the runtime, which puts
// the game's bottom picture on it, and its touches go back there for the panels.
// The display: any one Android offers for a presentation that the game's window isn't on, or the built-in screen when the
// game runs on another one (the AYN Thor counts its top screen as the external one). The runtime is told whether there
// is one (the settings menu greys the setting out without one), again whenever a display comes or goes.
// The Presentation's window never takes the focus, so the controller's keys stay with the game; it still gets touches.
// It goes while the activity is paused and comes back with it.
package io.github.morerokk.mhgurecomp;

import android.app.Activity;
import android.app.Presentation;
import android.content.Context;
import android.hardware.display.DisplayManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Display;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;

final class DualScreen {
    private static final String TAG = "MHGURecomp";
    private static Activity activity;
    private static DisplayManager displays;
    private static final Handler ui = new Handler(Looper.getMainLooper());
    private static boolean wanted, resumed, reported, reportedValue;
    private static Panel panel;

    static native void nativeAvailable(boolean yes);
    static native void nativeSurface(Surface surface, int width, int height);
    static native void nativeTouch(boolean down, float x, float y);

    // From MHGUActivity.onCreate, after SDL loaded the runtime (the native methods are in it).
    static void init(Activity a) {
        activity = a;
        displays = (DisplayManager) a.getSystemService(Context.DISPLAY_SERVICE);
        if (displays == null) return;
        displays.registerDisplayListener(new DisplayManager.DisplayListener() {
            @Override public void onDisplayAdded(int id) { update(); }
            @Override public void onDisplayRemoved(int id) { update(); }
            @Override public void onDisplayChanged(int id) { update(); }
        }, ui);
        update();
    }

    static void resumed(boolean yes) {
        resumed = yes;
        update();
    }

    // The runtime ([Dual Screen] SecondDisplay at the start, and after the settings menu's Save): any thread.
    static void want(final boolean on) {
        ui.post(new Runnable() {
            @Override public void run() {
                wanted = on;
                update();
            }
        });
    }

    // The display the game's window is on.
    private static int gameDisplay() {
        Display d = null;
        if (Build.VERSION.SDK_INT >= 30) {
            try { d = activity.getDisplay(); } catch (UnsupportedOperationException e) { d = null; }
        }
        if (d == null) d = activity.getWindowManager().getDefaultDisplay();
        return d != null ? d.getDisplayId() : Display.DEFAULT_DISPLAY;
    }

    // The display for the bottom screen, or null: one Android offers for presentations that isn't the game's, else the
    // built-in screen when the game is on another one.
    private static Display pick() {
        if (displays == null || activity == null) return null;
        int game = gameDisplay();
        Display builtin = null;
        for (Display d : displays.getDisplays()) {
            if (d.getDisplayId() == game || !d.isValid() || (d.getFlags() & Display.FLAG_PRIVATE) != 0) continue;
            if ((d.getFlags() & Display.FLAG_PRESENTATION) != 0) return d;
            if (d.getDisplayId() == Display.DEFAULT_DISPLAY) builtin = d;
        }
        return builtin;
    }

    private static void update() {
        if (activity == null) return;
        Display d = pick();
        boolean there = d != null;
        if (!reported || reportedValue != there) {
            reported = true;
            reportedValue = there;
            try {
                nativeAvailable(there);
            } catch (UnsatisfiedLinkError e) {
                reported = false;  // (the runtime isn't loaded yet: told at the next change)
            }
        }
        Display show = wanted && resumed ? d : null;
        if (panel != null && (show == null || panel.getDisplay().getDisplayId() != show.getDisplayId())) {
            panel.dismiss();  // (its surface goes: surfaceDestroyed tells the runtime)
            panel = null;
        }
        if (show != null && panel == null && !activity.isFinishing()) {
            Panel p = new Panel(activity, show);
            try {
                p.show();
                panel = p;
                Log.i(TAG, "bottom screen on display " + show.getDisplayId() + " (" + show.getName() + ")");
            } catch (WindowManager.InvalidDisplayException e) {
                Log.w(TAG, "bottom screen: display " + show.getDisplayId() + " can't be used", e);
            }
        }
    }

    private static final class Panel extends Presentation {
        Panel(Activity a, Display d) {
            super(a, d);
        }

        @Override
        protected void onCreate(Bundle saved) {
            super.onCreate(saved);
            Window w = getWindow();
            if (w != null) {
                w.addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON
                           | WindowManager.LayoutParams.FLAG_FULLSCREEN | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN);
                if (Build.VERSION.SDK_INT >= 28)
                    w.getAttributes().layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
                w.getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                                                       | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
            }
            SurfaceView view = new SurfaceView(getContext());
            view.getHolder().addCallback(new SurfaceHolder.Callback() {
                @Override public void surfaceCreated(SurfaceHolder h) {}
                @Override public void surfaceChanged(SurfaceHolder h, int format, int width, int height) {
                    nativeSurface(h.getSurface(), width, height);
                }
                @Override public void surfaceDestroyed(SurfaceHolder h) {
                    nativeSurface(null, 0, 0);
                }
            });
            view.setOnTouchListener(new View.OnTouchListener() {
                @Override public boolean onTouch(View v, MotionEvent e) {
                    int a = e.getActionMasked();
                    // (one finger: the first one down; MHXX's panels take one touch at a time)
                    boolean down = a != MotionEvent.ACTION_UP && a != MotionEvent.ACTION_CANCEL;
                    nativeTouch(down, e.getX(0), e.getY(0));
                    return true;
                }
            });
            setContentView(view);
            MHGUActivity.hideFromAccessibility(getWindow());  // (no green accessibility focus box on it either)
        }
    }
}
