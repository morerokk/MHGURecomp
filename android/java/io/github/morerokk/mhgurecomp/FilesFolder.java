// The app's files folder: where the settings, saves, mods, cheats, screenshots and log.txt are. The app's own folder
// (Android/data/<package>/files, which file managers on the device can't open since Android 11) unless the player chose
// another one in the settings' Android tab (Files folder): its path is in files-folder.txt in the app's own folder, and
// it is used while the app can write to it ("All files access"). runtime/platform/android.c android_home reads the same.
package io.github.morerokk.mhgurecomp;

import android.content.Context;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;

final class FilesFolder {
    static final String POINTER = "files-folder.txt";

    static File get(Context c) {
        File app = c.getExternalFilesDir(null);
        if (app == null) return null;
        try (BufferedReader r = new BufferedReader(new FileReader(new File(app, POINTER)))) {
            String line = r.readLine();
            if (line != null && !line.trim().isEmpty()) {
                File dir = new File(line.trim());
                if (dir.isDirectory() && dir.canWrite()) return dir;
            }
        } catch (IOException e) {
            // (none chosen)
        }
        return app;
    }

    private FilesFolder() {}
}
