package com.sbro.emucorea.core

import android.content.Intent
import android.net.Uri
import android.os.Environment
import android.provider.DocumentsContract
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import java.io.File
import org.junit.After
import org.junit.Before
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class DocumentPathResolverInstrumentedTest {
    private val context = InstrumentationRegistry.getInstrumentation().targetContext
    private val grants = mutableListOf<Pair<Uri, Int>>()
    private val authority = "com.sbro.emucorea.test.documents"
    private val rootPath = File(Environment.getExternalStorageDirectory(), "ResolverTest").absolutePath

    @Before
    fun clearTestGrants() {
        context.contentResolver.persistedUriPermissions.filter { it.uri.authority == authority }.forEach {
            val flags = (if (it.isReadPermission) Intent.FLAG_GRANT_READ_URI_PERMISSION else 0) or
                (if (it.isWritePermission) Intent.FLAG_GRANT_WRITE_URI_PERMISSION else 0)
            context.contentResolver.releasePersistableUriPermission(it.uri, flags)
            context.revokeUriPermission(it.uri, flags)
        }
    }

    private fun persist(uri: Uri, flags: Int = Intent.FLAG_GRANT_READ_URI_PERMISSION) {
        context.grantUriPermission(context.packageName, uri, flags or Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION)
        context.contentResolver.takePersistableUriPermission(uri, flags)
        grants += uri to flags
    }

    @After
    fun releaseGrants() {
        for ((uri, flags) in grants) {
            context.contentResolver.releasePersistableUriPermission(uri, flags)
            context.revokeUriPermission(uri, flags)
        }
    }

    @Test
    fun singleDocumentGrantIsNotTraversedAsDirectory() {
        val single = DocumentsContract.buildDocumentUri(authority, "primary:ResolverTest/emucorea-document-path-resolver-fixture.iso")
        persist(single)
        assertNull(DocumentPathResolver.findAccessibleTreeUriForRawPath(context, "$rootPath/emucorea-document-path-resolver-fixture.iso/child.iso"))
        assertNull(DocumentPathResolver.findAccessibleTreeUriForRawPath(context, "$rootPath/emucorea-document-path-resolver-fixture.iso"))
    }

    @Test
    fun contentFallbackSkipsSingleDocumentAndMalformedTreeGrants() {
        persist(DocumentsContract.buildDocumentUri(authority, "primary:ResolverTest/emucorea-document-path-resolver-fixture.iso"))
        persist(Uri.parse("content://$authority/tree"))
        assertNull(DocumentPathResolver.resolveFilePath(context, "content://$authority/file/selected"))
    }

    @Test
    fun readableTreeStillResolvesRootAndChildWithMixedGrants() {
        persist(DocumentsContract.buildDocumentUri(authority, "primary:ResolverTest/emucorea-document-path-resolver-fixture.iso"))
        val tree = DocumentsContract.buildTreeDocumentUri(authority, "primary:ResolverTest")
        persist(tree)
        assertEquals(tree, DocumentPathResolver.findAccessibleTreeUriForRawPath(context, rootPath))
        val child = DocumentsContract.buildDocumentUriUsingTree(tree, "primary:ResolverTest/emucorea-document-path-resolver-fixture.iso")
        assertEquals(child, DocumentPathResolver.findAccessibleTreeUriForRawPath(context, "$rootPath/emucorea-document-path-resolver-fixture.iso"))
        assertEquals("$rootPath/emucorea-document-path-resolver-fixture.iso",
            DocumentPathResolver.resolveFilePath(context, "content://$authority/file/selected"))
    }

    @Test
    fun writeOnlyTreeIsNotReturnedAsReadableDirectory() {
        persist(DocumentsContract.buildTreeDocumentUri(authority, "primary:ResolverTest"),
            Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
        assertNull(DocumentPathResolver.findAccessibleTreeUriForRawPath(context, rootPath))
        assertNull(DocumentPathResolver.resolveFilePath(context, "content://$authority/file/selected"))
    }
}

