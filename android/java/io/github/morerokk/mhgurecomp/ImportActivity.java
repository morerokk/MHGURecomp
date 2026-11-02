// Copies the game files or a save into the app's folder from a folder or files the player picks (the system's own
// picker: any folder on the device, an SD card, Downloads...). Android/data can't be written by the player's file
// managers since Android 11, and files the app made there can't be replaced by them: the app copies them in itself.
// Also the other way, a save out to a folder (EXPORT), and a folder to keep the save in (FOLDER: a folder syncing software keeps in step,
// say; the app then reads and writes the save there itself, which takes Android's "All files access").
//
// Started by runtime/platform/android.c (begin), which waits for state() to leave RUNNING. Its own window is see
// through: only its dialogs show, over the game.
package io.github.morerokk.mhgurecomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ContentResolver;
import android.content.DialogInterface;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.StatFs;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.view.View;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

public class ImportActivity extends Activity {
    public static final int GAME = 1, SAVE = 2, EXPORT = 3, FOLDER = 4;
    public static final int RUNNING = 0, DONE = 1, CANCELLED = 2, FAILED = 3;
    static final long SAVE_SIZE = 0x4eb8bcL;  // (a save's system file: runtime/services/bonus.c)
    static final int PICK = 1, ALL_FILES = 2;

    private static volatile int state = CANCELLED;
    private static int mode;
    private static String dest;  // GAME: the data folder (exefs/, romfs/ go in it); SAVE, EXPORT: the save's folder
    private static volatile String picked = "";  // FOLDER: the path of the folder chosen
    private static boolean closes;  // the game is running: the app closes after the import

    // From the native side, any thread. what: GAME or SAVE; folder: see dest; running: the game has started.
    public static void begin(Activity from, int what, String folder, boolean running) {
        mode = what;
        dest = folder;
        closes = running;
        state = RUNNING;
        from.startActivity(new Intent(from, ImportActivity.class));
    }

    public static int state() { return state; }
    public static String picked() { return picked; }

    private String title() {
        return mode == GAME ? "Import game files" : mode == SAVE ? "Import save" : mode == EXPORT ? "Export save" : "Save folder";
    }

    private volatile boolean stop;
    private AlertDialog progress;

    private AlertDialog.Builder dialog() {
        return new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert).setCancelable(false);
    }

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        if (saved != null) { end(CANCELLED); return; }  // (made again by the system: its dialogs are gone)
        picked = "";
        String text = mode == GAME
            ? "Pick the folder that holds your dump's exefs and romfs folders (for example in Download). "
              + "They are copied into the app, replacing the game files it has. This takes a few minutes."
            : mode == SAVE
            ? "Pick your save's system file, and its system_backup file if you have one (the files you'd use on "
              + "PC, or from a Switch save manager). Your current save is kept in a save_0.before-import folder."
            : mode == EXPORT
            ? "Pick the folder to copy your save's system and system_backup files into (for example in Download). "
              + "Files of those names that are already there are replaced."
            : "Pick the folder the save is kept in from now on: the one that holds (or will hold) save_0, as the PC "
              + "version's save folder does. Made so syncing software can keep the save the same on your devices: point it at the "
              + "same folder on each. The app reads and writes it itself, so Android asks you to allow \"All files access\" "
              + "for it. Your current save is not moved: use Export save first if you want it there.";
        if (closes && mode <= SAVE) text += "\n\nThe app closes when the import is done: start it again to play.";
        dialog().setTitle(title()).setMessage(text)
            .setPositiveButton("Choose", on(0))
            .setNegativeButton("Cancel", on(CANCELLED))
            .show();
    }

    private void pick() {
        Intent i;
        if (mode != SAVE) {
            i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        } else {
            i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        }
        try {
            startActivityForResult(i, PICK);
        } catch (Exception e) {
            fail("This device has no file picker (" + e + ").");
        }
    }

    @Override
    protected void onActivityResult(int request, int result, final Intent data) {
        super.onActivityResult(request, result, data);
        if (request == ALL_FILES) {  // back from Android's settings page for "All files access"
            if (allFiles()) finishFolder(); else fail("Without \"All files access\" the app can't use that folder, so the save stays where it was.");
            return;
        }
        if (request != PICK) return;
        if (result != RESULT_OK || data == null) { end(CANCELLED); return; }
        if (mode == FOLDER) { folderPicked(data.getData()); return; }
        progress = dialog().setTitle(title()).setMessage("Starting...")
            .setNegativeButton("Stop", null).show();
        progress.getButton(AlertDialog.BUTTON_NEGATIVE).setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                stop = true;
                show("Stopping...");
            }
        });
        new Thread(new Runnable() {
            @Override public void run() {
                String error;
                try {
                    error = mode == GAME ? importGame(data.getData()) : mode == EXPORT ? exportSave(data.getData()) : importSave(data);
                } catch (Exception e) {
                    error = "The copy failed: " + e.getMessage();
                }
                final String err = error;
                runOnUiThread(new Runnable() {
                    @Override public void run() {
                        progress.dismiss();
                        if (stop && err != null) end(CANCELLED);
                        else if (err != null) fail(err);
                        else done(mode == GAME ? "The game files are in." : mode == EXPORT ? "The save is exported." : "The save is in.");
                    }
                });
            }
        }, "import").start();
    }

    private void show(final String text) {
        runOnUiThread(new Runnable() {
            @Override public void run() { if (progress != null) progress.setMessage(text); }
        });
    }

    // A dialog button: 0 opens the picker, else ends the import so (no lambdas: the SDK's android.jar has no
    // LambdaMetafactory for javac)
    private DialogInterface.OnClickListener on(final int how) {
        return new DialogInterface.OnClickListener() {
            @Override public void onClick(DialogInterface d, int w) {
                if (how == 0) pick();
                else end(how);
            }
        };
    }

    private void fail(String text) {
        dialog().setTitle("Import failed").setMessage(text).setPositiveButton("OK", on(FAILED)).show();
    }

    private void done(String text) {
        if (closes && mode <= SAVE) text += " The app closes now: start it again to play.";
        dialog().setTitle(title()).setMessage(text)
            .setPositiveButton("OK", on(DONE)).show();
    }

    private void end(int how) {
        state = how;
        finish();
    }

    // ------------------------------------------------------------------ game files
    static class Item { String id, name, mime; long size; Item(String i, String n, String m, long s) { id = i; name = n; mime = m; size = s; } }
    static class Entry { String id, path; long size; Entry(String i, String p, long s) { id = i; path = p; size = s; } }

    private List<Item> children(Uri tree, String id) {
        List<Item> out = new ArrayList<>();
        String[] cols = {Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE, Document.COLUMN_SIZE};
        try (Cursor c = getContentResolver().query(DocumentsContract.buildChildDocumentsUriUsingTree(tree, id), cols, null, null, null)) {
            while (c != null && c.moveToNext())
                out.add(new Item(c.getString(0), c.getString(1), c.getString(2), c.isNull(3) ? 0 : c.getLong(3)));
        }
        return out;
    }

    private static Item named(List<Item> items, String name, boolean dir) {
        for (Item i : items)
            if (i.name != null && i.name.equalsIgnoreCase(name) && dir == Document.MIME_TYPE_DIR.equals(i.mime)) return i;
        return null;
    }

    private int found;

    private void walk(Uri tree, String id, String path, List<Entry> out) throws IOException {
        for (Item i : children(tree, id)) {
            if (stop) throw new IOException("stopped");
            String p = path + "/" + i.name;
            if (Document.MIME_TYPE_DIR.equals(i.mime)) walk(tree, i.id, p, out);
            else out.add(new Entry(i.id, p, i.size));
            if (++found % 500 == 0) show("Looking at the files: " + found + " so far...");
        }
    }

    private String importGame(Uri tree) throws IOException {
        // the folder picked, or one inside it (data/, game/...), holds exefs/ and romfs/
        String root = DocumentsContract.getTreeDocumentId(tree);
        List<Item> top = children(tree, root);
        Item exefs = named(top, "exefs", true), romfs = named(top, "romfs", true);
        for (Item sub : top) {
            if (exefs != null && romfs != null) break;
            if (!Document.MIME_TYPE_DIR.equals(sub.mime)) continue;
            List<Item> in = children(tree, sub.id);
            if (named(in, "exefs", true) != null && named(in, "romfs", true) != null) {
                exefs = named(in, "exefs", true);
                romfs = named(in, "romfs", true);
            }
        }
        if (exefs == null || romfs == null)
            return "That folder has no exefs and romfs folders in it. Pick the folder that holds both.";
        if (named(children(tree, exefs.id), "main", false) == null)
            return "The exefs folder has no main file in it: it isn't a complete dump.";
        List<Entry> files = new ArrayList<>();
        walk(tree, exefs.id, "exefs", files);
        walk(tree, romfs.id, "romfs", files);
        long total = 0;
        for (Entry e : files) total += e.size;

        File data = new File(dest), part = new File(data, "import.part");
        File oldExefs = new File(data, "exefs"), oldRomfs = new File(data, "romfs");
        delete(part);
        data.mkdirs();
        long free = new StatFs(data.getPath()).getAvailableBytes(), margin = 200L << 20;
        if (free < total + margin) {  // (the old game files go first when that makes room: they are being replaced)
            long old = size(oldExefs) + size(oldRomfs);
            if (free + old < total + margin)
                return "Not enough free space: the game files need " + gb(total) + " and the device has " + gb(free + old) + ".";
            show("Removing the old game files...");
            delete(oldExefs);
            delete(oldRomfs);
        }
        byte[] buf = new byte[1 << 20];
        long copied = 0, shown = 0;
        int n = 0;
        ContentResolver cr = getContentResolver();
        try {
            for (Entry e : files) {
                File to = new File(part, e.path);
                to.getParentFile().mkdirs();
                try (InputStream in = cr.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, e.id));
                     OutputStream out = new FileOutputStream(to)) {
                    if (in == null) throw new IOException("can't read " + e.path);
                    for (int r; (r = in.read(buf)) > 0; ) {
                        if (stop) throw new IOException("stopped");
                        out.write(buf, 0, r);
                        copied += r;
                        if (copied - shown > (64L << 20)) {
                            shown = copied;
                            show("Copying: " + gb(copied) + " of " + gb(total) + " (" + n + " of " + files.size() + " files)");
                        }
                    }
                }
                n++;
            }
            show("Finishing...");
            delete(oldExefs);
            delete(oldRomfs);
            if (!new File(part, "exefs").renameTo(oldExefs) || !new File(part, "romfs").renameTo(oldRomfs))
                throw new IOException("can't move the files into " + data);
        } catch (IOException e) {
            delete(part);
            throw e;
        }
        delete(part);
        return null;
    }

    // ------------------------------------------------------------------ save out, and the save's folder
    private String exportSave(Uri tree) throws IOException {
        File save = new File(dest);
        File[] have = {new File(save, "system"), new File(save, "system_backup")};
        if (!have[0].isFile()) return "There is no save yet to export (" + have[0] + ").";
        ContentResolver cr = getContentResolver();
        String root = DocumentsContract.getTreeDocumentId(tree);
        Uri dir = DocumentsContract.buildDocumentUriUsingTree(tree, root);
        List<Item> there = children(tree, root);
        byte[] buf = new byte[1 << 16];
        for (File f : have) {
            if (!f.isFile()) continue;
            Item old = named(there, f.getName(), false);
            if (old != null) DocumentsContract.deleteDocument(cr, DocumentsContract.buildDocumentUriUsingTree(tree, old.id));
            Uri out = DocumentsContract.createDocument(cr, dir, "application/octet-stream", f.getName());
            if (out == null) return "Can't write to that folder: pick another one.";
            try (InputStream in = new java.io.FileInputStream(f); OutputStream o = cr.openOutputStream(out)) {
                if (o == null) throw new IOException("can't write " + f.getName());
                for (int r; (r = in.read(buf)) > 0; ) o.write(buf, 0, r);
            }
        }
        return null;
    }

    private String folderPath;

    // The folder's path as the app can use it, from the picker's document id ("primary:Sync/MHGU" is
    // /storage/emulated/0/Sync/MHGU; "1234-ABCD:Sync" an SD card's /storage/1234-ABCD/Sync).
    private void folderPicked(Uri tree) {
        String id = DocumentsContract.getTreeDocumentId(tree);
        int colon = id.indexOf(':');
        String vol = colon < 0 ? id : id.substring(0, colon), rel = colon < 0 ? "" : id.substring(colon + 1);
        String path;
        if (vol.equals("primary")) path = Environment.getExternalStorageDirectory().getPath() + (rel.isEmpty() ? "" : "/" + rel);
        else if (vol.equals("raw")) path = rel;
        else if (vol.matches("[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}")) path = "/storage/" + vol + (rel.isEmpty() ? "" : "/" + rel);
        else { fail("That folder can't be used (" + id + "): pick one in the device's storage or on an SD card."); return; }
        if (path.contains("/Android/data") || path.contains("/Android/obb")) {
            fail("Android doesn't let apps use other apps' Android/data folders: pick another folder."); return;
        }
        folderPath = path;
        if (Build.VERSION.SDK_INT < 30) { fail("This needs Android 11 or newer."); return; }
        if (allFiles()) { finishFolder(); return; }
        dialog().setTitle(title())
            .setMessage("To read and write the save in that folder itself, the app needs \"All files access\". The next screen "
                        + "is Android's own: turn it on for MHGU Recomp, then go back.")
            .setPositiveButton("Continue", new DialogInterface.OnClickListener() {
                @Override public void onClick(DialogInterface d, int w) {
                    try {
                        startActivityForResult(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                            Uri.parse("package:" + getPackageName())), ALL_FILES);
                    } catch (Exception e) {
                        fail("Can't open Android's settings (" + e + ").");
                    }
                }
            })
            .setNegativeButton("Cancel", on(CANCELLED)).show();
    }

    private boolean allFiles() { return Build.VERSION.SDK_INT >= 30 && Environment.isExternalStorageManager(); }

    private void finishFolder() {
        File dir = new File(folderPath);
        dir.mkdirs();
        if (!dir.isDirectory() || !dir.canWrite()) {
            fail("The app can't write to " + folderPath + ".");
            return;
        }
        picked = folderPath;
        done("The save will be kept in " + folderPath + ". Press Save in the menu, then restart the app to use it.");
    }

    // ------------------------------------------------------------------ save
    private String importSave(Intent data) throws IOException {
        List<Uri> picked = new ArrayList<>();
        if (data.getData() != null) picked.add(data.getData());
        ClipData clip = data.getClipData();
        if (clip != null)
            for (int i = 0; i < clip.getItemCount(); i++)
                if (!picked.contains(clip.getItemAt(i).getUri())) picked.add(clip.getItemAt(i).getUri());
        Map<String, Uri> files = new HashMap<>();
        for (Uri u : picked) {
            String name = null;
            long size = -1;
            try (Cursor c = getContentResolver().query(u, new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE}, null, null, null)) {
                if (c != null && c.moveToFirst()) {
                    name = c.getString(0);
                    if (!c.isNull(1)) size = c.getLong(1);
                }
            }
            // (system and system_backup; a download may have become "system (1)" or "system.bin")
            String as = name == null ? null : name.toLowerCase(Locale.ROOT).startsWith("system")
                ? (name.toLowerCase(Locale.ROOT).contains("backup") ? "system_backup" : "system") : null;
            if (as == null) return "\"" + name + "\" isn't a save file: pick the files named system and system_backup.";
            if (size >= 0 && size != SAVE_SIZE)
                return "\"" + name + "\" isn't a Monster Hunter Generations Ultimate save (" + size + " bytes, a save has " + SAVE_SIZE + ").";
            if (files.containsKey(as)) return "Two of the files picked are both the save's " + as + " file: pick one of each.";
            files.put(as, u);
        }
        if (!files.containsKey("system")) return "Pick the save's system file too (system_backup alone isn't enough).";

        File save = new File(dest), part = new File(save.getParentFile(), save.getName() + ".import.part");
        delete(part);
        part.mkdirs();
        byte[] buf = new byte[1 << 16];
        try {
            for (Map.Entry<String, Uri> f : files.entrySet()) {
                long n = 0;
                try (InputStream in = getContentResolver().openInputStream(f.getValue());
                     OutputStream out = new FileOutputStream(new File(part, f.getKey()))) {
                    if (in == null) throw new IOException("can't read the " + f.getKey() + " file");
                    for (int r; (r = in.read(buf)) > 0; n += r) out.write(buf, 0, r);
                }
                if (n != SAVE_SIZE) throw new IOException("the " + f.getKey() + " file isn't a save (" + n + " bytes)");
            }
            if (save.exists()) {
                String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(new Date());
                File keep = new File(save.getParentFile(), save.getName() + ".before-import-" + stamp);
                if (!save.renameTo(keep)) throw new IOException("can't move the current save aside");
            }
            if (!part.renameTo(save)) throw new IOException("can't move the save into " + save);
        } catch (IOException e) {
            delete(part);
            throw e;
        }
        return null;
    }

    // ------------------------------------------------------------------ files
    private static void delete(File f) {
        File[] in = f.listFiles();
        if (in != null) for (File i : in) delete(i);
        f.delete();
    }

    private static long size(File f) {
        File[] in = f.listFiles();
        if (in == null) return f.length();
        long n = 0;
        for (File i : in) n += size(i);
        return n;
    }

    private static String gb(long n) { return String.format(Locale.ROOT, "%.1f GB", n / 1e9); }
}
