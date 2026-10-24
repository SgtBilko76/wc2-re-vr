package com.bilko.wc2vr;

import android.Manifest;
import android.app.AlarmManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
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
    private static final int STORAGE_REQUEST = 1;
    private static final String TAG = "WC2VR";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        if (Build.VERSION.SDK_INT >= 30 && !Environment.isExternalStorageManager()) {
            Log.w(TAG, "requesting all-files access for /sdcard/WingCommander");
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.parse("package:" + getPackageName())));
            } catch (Exception e) {
                try {
                    startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                } catch (Exception e2) {
                    // some headset OS builds have no such page: fall back to the app's settings
                    try {
                        startActivity(new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                                Uri.parse("package:" + getPackageName())));
                    } catch (Exception e3) {
                        Log.e(TAG, "cannot open the storage permission screen", e3);
                    }
                }
            }
        } else if (Build.VERSION.SDK_INT < 30
                && checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE)
                        != PackageManager.PERMISSION_GRANTED) {
            // Android 10 (PICO OS 5): no all-files access, ask for storage instead.
            Log.w(TAG, "requesting storage permission for /sdcard/WingCommander");
            requestPermissions(new String[] {
                    Manifest.permission.READ_EXTERNAL_STORAGE,
                    Manifest.permission.WRITE_EXTERNAL_STORAGE }, STORAGE_REQUEST);
        }
        super.onCreate(savedInstanceState);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions,
                                           int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode != STORAGE_REQUEST)
            return;
        if (grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
            // The game already looked for its data without access: start over.
            Log.i(TAG, "storage permission granted, restarting");
            Intent restart = getPackageManager().getLaunchIntentForPackage(getPackageName());
            if (restart != null) {
                restart.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
                PendingIntent pending = PendingIntent.getActivity(this, 0, restart,
                        PendingIntent.FLAG_CANCEL_CURRENT | PendingIntent.FLAG_IMMUTABLE);
                AlarmManager alarm = (AlarmManager) getSystemService(Context.ALARM_SERVICE);
                alarm.set(AlarmManager.RTC, System.currentTimeMillis() + 500, pending);
            }
            android.os.Process.killProcess(android.os.Process.myPid());
        } else {
            Log.w(TAG, "storage permission denied");
        }
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
