package com.doktorplopz.vrcore.host;

import java.io.File;
import java.util.ArrayList;

import android.content.Intent;
import android.content.UriPermission;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
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
 *
 * THE SOUNDTRACK'S FOLDER (called by vrcore's audio/storage.cpp). The shared Music/Soundtracks is read by path once
 * READ_MEDIA_AUDIO is granted (asked through SDL; permissionState says where it stands). Refused, the player can
 * choose the folder in the system picker instead (pickFolder): a document tree whose read grant is kept across
 * launches, listed with listFolder and its files opened by descriptor with openFd. Both 2D panels bring the app back
 * into VR when they close, as the file picker does.
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

    // THE SOUNDTRACK'S FOLDER: the shared folder's path, from any thread
    public String musicFolder() {
        File music = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_MUSIC);
        return music != null ? new File(music, "Soundtracks").getAbsolutePath() : null;
    }

    // 0 granted, 1 not granted (asking shows the request, or tells at once that it will not), 2 refused before
    public int permissionState(String permission) {
        if (checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED) return 0;
        return shouldShowRequestPermissionRationale(permission) ? 2 : 1;
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        // the request was a panel (we lost the focus to it): back into VR. Answered at once, nothing showed.
        if (!hasWindowFocus()) backToVr();
    }

    private static final int PICK_FOLDER = 0x5354;
    private final Object folderLock = new Object();
    private boolean folderPicking;
    private String folderPicked;        // the answer until taken: a tree URI, or "" (cancelled)

    // From any thread: false when the picker cannot be opened, or is open already.
    public boolean pickFolder() {
        synchronized (folderLock) {
            if (folderPicking) return false;
            folderPicking = true;
            folderPicked = null;
        }
        runOnUiThread(new Runnable() {
            public void run() {
                try {
                    Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
                    pick.putExtra(DocumentsContract.EXTRA_INITIAL_URI, DocumentsContract.buildDocumentUri(
                            "com.android.externalstorage.documents", "primary:Music/Soundtracks"));
                    pick.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
                    startActivityForResult(pick, PICK_FOLDER);
                } catch (Exception e) {
                    Log.e("vrcore", "could not open the folder picker", e);
                    synchronized (folderLock) {
                        folderPicking = false;
                        folderPicked = "";
                    }
                }
            }
        });
        return true;
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_FOLDER) return;
        String picked = "";
        Uri tree = resultCode == RESULT_OK && data != null ? data.getData() : null;
        if (tree != null) {
            try {
                getContentResolver().takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION);
                picked = tree.toString();
            } catch (Exception e) {
                Log.e("vrcore", "could not keep the grant on " + tree, e);
            }
        }
        synchronized (folderLock) {
            folderPicking = false;
            folderPicked = picked;
        }
        backToVr();
    }

    // The picker's answer once (a tree URI, or "" when cancelled); null while there is none.
    public String takeFolderPick() {
        synchronized (folderLock) {
            String r = folderPicked;
            folderPicked = null;
            return r;
        }
    }

    // The read grant on a picked tree is still held.
    public boolean folderGranted(String uri) {
        for (UriPermission p : getContentResolver().getPersistedUriPermissions())
            if (p.isReadPermission() && p.getUri().toString().equals(uri)) return true;
        return false;
    }

    // A picked tree's folder (the tree itself, or a document under it): "d" or "f", a tab, the name, a tab, the
    // document's URI, one an entry; null when it cannot be read.
    public String[] listFolder(String uri) {
        try {
            Uri u = Uri.parse(uri);
            String id = DocumentsContract.isDocumentUri(this, u) ? DocumentsContract.getDocumentId(u)
                                                                  : DocumentsContract.getTreeDocumentId(u);
            Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(u, id);
            ArrayList<String> out = new ArrayList<>();
            try (Cursor c = getContentResolver().query(children, new String[] {
                    DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                    DocumentsContract.Document.COLUMN_MIME_TYPE}, null, null, null)) {
                if (c == null) return null;
                while (c.moveToNext()) {
                    boolean dir = DocumentsContract.Document.MIME_TYPE_DIR.equals(c.getString(2));
                    Uri doc = DocumentsContract.buildDocumentUriUsingTree(u, c.getString(0));
                    out.add((dir ? "d" : "f") + "\t" + c.getString(1) + "\t" + doc);
                }
            }
            return out.toArray(new String[0]);
        } catch (Exception e) {
            Log.w("vrcore", "cannot list " + uri, e);
            return null;
        }
    }

    // A document's descriptor for reading (the caller closes it); -1 when it cannot be opened.
    public int openFd(String uri) {
        try {
            ParcelFileDescriptor p = getContentResolver().openFileDescriptor(Uri.parse(uri), "r");
            return p != null ? p.detachFd() : -1;
        } catch (Exception e) {
            Log.w("vrcore", "cannot open " + uri, e);
            return -1;
        }
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
