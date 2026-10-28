package com.sbro.emucorea.core;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import java.io.FileNotFoundException;
import java.io.InputStream;
import java.io.OutputStream;

/** Serves a test archive as a non-seekable stream, like some cloud providers. */
public class TestArchiveStreamProvider extends ContentProvider {
    @Override public boolean onCreate() { return true; }
    @Override public String getType(Uri uri) { return "application/zip"; }
    @Override public Cursor query(Uri uri, String[] projection, String selection, String[] args, String order) {
        String[] columns = projection != null ? projection : new String[]{OpenableColumns.DISPLAY_NAME};
        MatrixCursor cursor = new MatrixCursor(columns);
        Object[] row = new Object[columns.length];
        for (int i = 0; i < columns.length; i++) if (OpenableColumns.DISPLAY_NAME.equals(columns[i])) row[i] = "games.zip";
        cursor.addRow(row);
        return cursor;
    }
    @Override public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        try {
            ParcelFileDescriptor[] pipe = ParcelFileDescriptor.createPipe();
            new Thread(() -> {
                try (InputStream input = getContext().getAssets().open("archive-fixtures/games.zip");
                     OutputStream output = new ParcelFileDescriptor.AutoCloseOutputStream(pipe[1])) {
                    byte[] buffer = new byte[8192];
                    int count;
                    while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
                } catch (Exception ignored) { }
            }).start();
            return pipe[0];
        } catch (Exception e) { throw new FileNotFoundException(e.toString()); }
    }
    @Override public Uri insert(Uri uri, ContentValues values) { return null; }
    @Override public int delete(Uri uri, String selection, String[] args) { return 0; }
    @Override public int update(Uri uri, ContentValues values, String selection, String[] args) { return 0; }
}
