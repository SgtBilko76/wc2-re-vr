package com.bilko.wc2vr;

import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.util.Log;

import org.libsdl.app.SDLActivity;

/**
 * Wing Commander II VR: SDL2 hosts the reconstructed game on its own thread;
 * the native OpenXR thread (src/quest/quest_xr.c) presents it in the headset.
 * Game data is read from /sdcard/WingCommander (needs all-files access).
 */
public class WC2Activity extends SDLActivity {
    private static final String TAG = "WC2VR";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        if (Build.VERSION.SDK_INT >= 30 && !Environment.isExternalStorageManager()) {
            Log.w(TAG, "requesting all-files access for /sdcard/WingCommander");
            try {
                Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.parse("package:" + getPackageName()));
                startActivity(intent);
            } catch (Exception e) {
                Log.e(TAG, "cannot open the storage permission screen", e);
            }
        }
        super.onCreate(savedInstanceState);
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    @Override
    protected void onDestroy() {
        // SDL joins its native thread without a timeout; make sure the
        // process (and its audio) really ends.
        Thread watchdog = new Thread(() -> {
            try { Thread.sleep(3000); } catch (InterruptedException ignored) { }
            android.os.Process.killProcess(android.os.Process.myPid());
        });
        watchdog.setDaemon(true);
        watchdog.start();
        super.onDestroy();
        android.os.Process.killProcess(android.os.Process.myPid());
    }
}
