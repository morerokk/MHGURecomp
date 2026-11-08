// The logs as content:// links for LogActivity's share (Android gives no app a file:// path of another's): only
// log.txt and crash-log.txt of the app's files folder (FilesFolder), read only, to the app the player shared them with (a grant per share;
// not exported). In LogActivity's process.
package io.github.morerokk.mhgurecomp;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;

public class LogProvider extends ContentProvider {
    public static final String AUTHORITY = "io.github.morerokk.mhgurecomp.logs";
    public static final String LOG = "log.txt", CRASH_LOG = "crash-log.txt";

    public static Uri uri(String name) { return Uri.parse("content://" + AUTHORITY + "/" + name); }

    private File file(Uri uri) throws FileNotFoundException {
        String name = uri.getLastPathSegment();
        if (!LOG.equals(name) && !CRASH_LOG.equals(name)) throw new FileNotFoundException(String.valueOf(uri));
        File f = new File(FilesFolder.get(getContext()), name);
        if (!f.isFile()) throw new FileNotFoundException(f.getPath());
        return f;
    }

    @Override public boolean onCreate() { return true; }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)) throw new SecurityException("read only");
        return ParcelFileDescriptor.open(file(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] args, String order) {
        File f;
        try { f = file(uri); } catch (FileNotFoundException e) { return null; }
        if (projection == null) projection = new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE};
        MatrixCursor c = new MatrixCursor(projection, 1);
        Object[] row = new Object[projection.length];
        for (int i = 0; i < projection.length; i++)
            row[i] = OpenableColumns.DISPLAY_NAME.equals(projection[i]) ? "MHGU-Recomp-" + f.getName()
                   : OpenableColumns.SIZE.equals(projection[i]) ? (Object)f.length() : null;
        c.addRow(row);
        return c;
    }

    @Override public String getType(Uri uri) { return "text/plain"; }
    @Override public Uri insert(Uri uri, ContentValues values) { return null; }
    @Override public int delete(Uri uri, String selection, String[] args) { return 0; }
    @Override public int update(Uri uri, ContentValues values, String selection, String[] args) { return 0; }
}
