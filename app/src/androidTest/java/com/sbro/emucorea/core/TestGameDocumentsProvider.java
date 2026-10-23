package com.sbro.emucorea.core;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;

// This provider runs in the test APK's own process, without the app's Kotlin runtime.
// It exposes synthetic metadata only, never user files.
public class TestGameDocumentsProvider extends ContentProvider {
    @Override public boolean onCreate() { return true; }
    @Override public String getType(Uri uri) { return "application/octet-stream"; }
    @Override public Cursor query(Uri uri, String[] projection, String selection,
            String[] selectionArgs, String sortOrder) {
        String[] columns = projection != null ? projection : new String[] {
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                OpenableColumns.DISPLAY_NAME, DocumentsContract.Document.COLUMN_MIME_TYPE };
        MatrixCursor cursor = new MatrixCursor(columns);
        Object[] row = new Object[columns.length];
        for (int i = 0; i < columns.length; ++i) {
            switch (columns[i]) {
            case DocumentsContract.Document.COLUMN_DOCUMENT_ID:
                row[i] = "primary:ResolverTest/emucorea-document-path-resolver-fixture.iso";
                break;
            case OpenableColumns.DISPLAY_NAME:
                row[i] = "emucorea-document-path-resolver-fixture.iso";
                break;
            case DocumentsContract.Document.COLUMN_MIME_TYPE:
                row[i] = "application/octet-stream";
                break;
            }
        }
        cursor.addRow(row);
        return cursor;
    }
    @Override public Uri insert(Uri uri, ContentValues values) { return null; }
    @Override public int delete(Uri uri, String selection, String[] selectionArgs) { return 0; }
    @Override public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) { return 0; }
}
