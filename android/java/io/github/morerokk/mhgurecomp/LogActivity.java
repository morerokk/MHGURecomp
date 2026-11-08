// Shares log.txt with any app that takes files (Discord, email, a file manager...), without a computer. Two ways in:
//   - after a crash: the runtime leaves the "last-run-crashed" file (runtime/platform/android.c), and at the next start
//     MHGUActivity keeps that run's log as crash-log.txt before the game writes a new log.txt, then opens this with
//     CRASH, which asks whether to share it
//   - the "Share log" shortcut on the app icon (a long press: res/xml/shortcuts.xml), which shares log.txt (the last
//     run's, or the one being written) and never starts the game
// It runs in a process of its own (AndroidManifest.xml), so the dialog stays up if the game, starting behind it,
// crashes again. LogProvider hands the file to the other app.
package io.github.morerokk.mhgurecomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.DialogInterface;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.widget.Toast;

import java.io.File;

public class LogActivity extends Activity {
    public static final String CRASH = "io.github.morerokk.mhgurecomp.CRASH";

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        if (saved != null) { finish(); return; }  // (made again by the system: its dialog is gone)
        boolean crash = getIntent().getBooleanExtra(CRASH, false);
        final String name = crash ? LogProvider.CRASH_LOG : LogProvider.LOG;
        if (!new File(FilesFolder.get(this), name).isFile()) {  // (the app's files folder: LogProvider shares from it)
            Toast.makeText(this, "No log yet: start the game once first.", Toast.LENGTH_LONG).show();
            finish();
            return;
        }
        if (!crash) { share(name); return; }
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle("The game closed because of an error")
            .setMessage("Share its log (log.txt) so the problem can be looked at? You can send it to Discord, email or any "
                        + "app that takes files. It holds the game's messages from that start, nothing from your device.")
            .setCancelable(false)
            .setPositiveButton("Share log", new DialogInterface.OnClickListener() {
                public void onClick(DialogInterface d, int w) { share(name); }
            })
            .setNegativeButton("Not now", new DialogInterface.OnClickListener() {
                public void onClick(DialogInterface d, int w) { finish(); }
            })
            .show();
    }

    private void share(String name) {
        Uri uri = LogProvider.uri(name);
        Intent send = new Intent(Intent.ACTION_SEND);
        send.setType("text/plain");
        send.putExtra(Intent.EXTRA_STREAM, uri);
        send.putExtra(Intent.EXTRA_SUBJECT, "MHGU Recomp " + name);
        send.setClipData(ClipData.newRawUri(name, uri));  // (the read grant goes with the chooser to the app picked)
        send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        try {
            startActivity(Intent.createChooser(send, "Share " + name));
        } catch (RuntimeException e) {
            Toast.makeText(this, "No app on this device can take the file.", Toast.LENGTH_LONG).show();
        }
        finish();
    }
}
