package com.sbro.emucorea.data

import android.database.sqlite.SQLiteDatabase
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.sbro.emucorea.data.psp.PspCatalogRepository
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class PspCatalogMatchingInstrumentedTest {
    private fun match(methodName: String, title: String, vararg names: String): Long? {
        val repository = PspCatalogRepository(InstrumentationRegistry.getInstrumentation().targetContext)
        return SQLiteDatabase.create(null).use { db ->
            db.execSQL("CREATE TABLE games(igdb_id INTEGER, normalized_name TEXT)")
            names.forEachIndexed { index, name ->
                db.execSQL("INSERT INTO games VALUES(?, ?)", arrayOf<Any>(index + 1, name))
            }
            val method = PspCatalogRepository::class.java.getDeclaredMethod(methodName, SQLiteDatabase::class.java, List::class.java)
                .apply { isAccessible = true }
            method.invoke(repository, db, listOf(title)) as Long?
        }
    }

    private fun fuzzy(title: String, vararg names: String): Long? = match("findFuzzyTitleId", title, *names)

    @Test fun duplicateExactTitlesAreNotResolvedByDatabaseRowOrder() {
        assertNull(match("findExactTitleId", "example", "example", "example"))
    }

    @Test fun ambiguousFranchiseNameDoesNotSelectFirstDatabaseRow() {
        assertNull(fuzzy("god of war", "god of war chains of olympus", "god of war ghost of sparta"))
    }

    @Test fun shortenedNamesNeedStrongEnoughSimilarity() {
        assertNull(fuzzy("naruto ultimate", "naruto shippuden ultimate ninja impact"))
    }

    @Test fun romanAndArabicSequelNumbersStillMatch() {
        assertEquals(1L, fuzzy("final fantasy 4", "final fantasy iv"))
    }

    @Test fun bundledSerialIdentityWinsOverAnIncorrectTitle() {
        val repository = PspCatalogRepository(InstrumentationRegistry.getInstrumentation().targetContext)
        assertEquals(18787L, repository.findBestMatchId("ULUS-10001", "wrong title"))
    }
}
