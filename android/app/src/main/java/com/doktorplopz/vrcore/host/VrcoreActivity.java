package com.doktorplopz.vrcore.host;

import java.util.ArrayList;

import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import org.libsdl.app.SDLActivity;

/**
 * The app's activity: vrcore's, the same in every game's APK (each APK names itself: android/app/build.gradle).
 * SDL loads libSDL3.so and libmain.so and runs SDL_main. Launch options come from the intent extra "args", split at spaces outside quotes: am start ... --es args "--foveation 3 --fdm-view",
 * or with a value that has spaces, --es args "--script --args '--course 2'".
 *
 * BACK INTO VR (backToVr, called by the native side when the system file picker closes: vrcore's ui/pick.cpp).
 * The picker is a 2D panel. When it closes, Android resumes the activity, but the Quest's shell leaves the player in
 * its own UI with the app's XR session visible and unfocused, until the app is started again from the Library. So
 * the app is started again here, with the intent the Library uses (the running instance comes to the front): 0.7 s
 * after the result, while the panel is still closing, then every 1.5 s until the activity has the window focus,
 * 4 times at most.
 *
 * A PICKED FILE'S NAME (displayName, called by ui/pick.cpp): the picker's URI may hold only a media id
 * (content://.../1000000640), so the name is the provider's DISPLAY_NAME; null when it has none.
 */
public class VrcoreActivity extends SDLActivity {
    @Override
    protected String[] getArguments() {
        String args = getIntent() != null ? getIntent().getStringExtra("args") : null;
        if (args == null) return new String[0];
        ArrayList<String> out = new ArrayList<>();
        StringBuilder word = new StringBuilder();
        boolean inWord = false;
        char quote = 0;
        for (int i = 0; i < args.length(); ++i) {
            char c = args.charAt(i);
            if (quote != 0) {
                if (c == quote) quote = 0;
                else word.append(c);
            } else if (c == '"' || c == '\'') {
                quote = c;
                inWord = true;
            } else if (Character.isWhitespace(c)) {
                if (inWord) out.add(word.toString());
                word.setLength(0);
                inWord = false;
            } else {
                word.append(c);
                inWord = true;
            }
        }
        if (inWord) out.add(word.toString());
        return out.toArray(new String[0]);
    }

    // A PICKED FILE'S NAME: from any thread
    public String displayName(String uri) {
        try (Cursor c = getContentResolver().query(Uri.parse(uri),
                new String[] {android.provider.OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) return c.getString(0);
        } catch (Exception e) {
            Log.w("vrcore", "no name for the picked file", e);
        }
        return null;
    }

    // BACK INTO VR: from any thread
    public void backToVr() {
        final Handler handler = new Handler(Looper.getMainLooper());
        handler.postDelayed(new Runnable() {
            int attempts;

            public void run() {
                if (attempts > 0 && hasWindowFocus()) return;
                if (attempts++ >= 4) {
                    Log.w("vrcore", "could not get back to VR after the picker");
                    return;
                }
                Log.i("vrcore", "back to VR after the picker (attempt " + attempts + ")");
                try {
                    Intent back = getPackageManager().getLaunchIntentForPackage(getPackageName());
                    if (back == null) back = new Intent(VrcoreActivity.this, VrcoreActivity.class);
                    back.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_REORDER_TO_FRONT);
                    startActivity(back);
                } catch (Exception e) {
                    Log.e("vrcore", "could not bring the app back to the front", e);
                }
                handler.postDelayed(this, 1500);
            }
        }, 700);
    }
}
