package com.sbro.emucorea.data

import android.Manifest
import android.app.NotificationChannel
import android.app.NotificationManager
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import com.google.android.gms.tasks.Task
import com.google.firebase.auth.FirebaseAuth
import com.google.firebase.firestore.DocumentSnapshot
import com.google.firebase.firestore.FieldValue
import com.google.firebase.firestore.FirebaseFirestore
import com.google.firebase.firestore.SetOptions
import com.sbro.emucorea.R
import kotlinx.coroutines.suspendCancellableCoroutine
import java.util.Locale
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

enum class EmuAchievementMetric {
    GamesPlayed,
    TotalPlayTimeMinutes,
    TotalSessions,
    PspGames,
    PspMinutes,
    PspSessions,
    LongestGameMinutes,
    MaxGameSessions,
    MatchingTitleMinutes,
    MatchingTitleSessions,
    MatchingGamesCount,
    GodOfWarMinutes,
    GtaLcsMinutes,
    TekkenDrMinutes
}

data class EmuAchievementDefinition(
    val id: String,
    val titleRes: Int,
    val descriptionRes: Int,
    val metric: EmuAchievementMetric,
    val target: Long,
    val points: Int,
    val hidden: Boolean = false,
    val textArgument: String? = null,
    val matchTerms: List<String> = emptyList(),
    val templatedText: Boolean = false
)

data class EmuAchievementState(
    val definition: EmuAchievementDefinition,
    val progress: Long,
    val unlockedAtMs: Long? = null
) {
    val unlocked: Boolean get() = unlockedAtMs != null
    val progressFraction: Float get() = (progress.toFloat() / definition.target.coerceAtLeast(1L)).coerceIn(0f, 1f)
}

internal fun resolveAchievementState(
    definition: EmuAchievementDefinition,
    progress: Long,
    cloudUnlockedAtMs: Long?,
    localCompletedAtMs: Long
): EmuAchievementState {
    val boundedProgress = progress.coerceAtMost(definition.target)
    return EmuAchievementState(
        definition = definition,
        progress = boundedProgress,
        unlockedAtMs = cloudUnlockedAtMs ?: localCompletedAtMs.takeIf { boundedProgress >= definition.target }
    )
}

object EmuCoreAAchievementCatalog {
    const val VERSION = 3

    val definitions = buildList {
        addAll(listOf(
        EmuAchievementDefinition("first_game", R.string.achievement_first_game_title, R.string.achievement_first_game_description, EmuAchievementMetric.GamesPlayed, 1, 10),
        EmuAchievementDefinition("five_games", R.string.achievement_five_games_title, R.string.achievement_five_games_description, EmuAchievementMetric.GamesPlayed, 5, 20),
        EmuAchievementDefinition("twenty_games", R.string.achievement_twenty_games_title, R.string.achievement_twenty_games_description, EmuAchievementMetric.GamesPlayed, 20, 50),
        EmuAchievementDefinition("one_hour", R.string.achievement_one_hour_title, R.string.achievement_one_hour_description, EmuAchievementMetric.TotalPlayTimeMinutes, 60, 15),
        EmuAchievementDefinition("ten_hours", R.string.achievement_ten_hours_title, R.string.achievement_ten_hours_description, EmuAchievementMetric.TotalPlayTimeMinutes, 600, 40),
        EmuAchievementDefinition("hundred_hours", R.string.achievement_hundred_hours_title, R.string.achievement_hundred_hours_description, EmuAchievementMetric.TotalPlayTimeMinutes, 6_000, 100),
        EmuAchievementDefinition("ten_sessions", R.string.achievement_ten_sessions_title, R.string.achievement_ten_sessions_description, EmuAchievementMetric.TotalSessions, 10, 20),
        EmuAchievementDefinition("ps1_debut", R.string.achievement_psp_debut_title, R.string.achievement_psp_debut_description, EmuAchievementMetric.PspGames, 1, 25),
        EmuAchievementDefinition("dedicated", R.string.achievement_dedicated_title, R.string.achievement_dedicated_description, EmuAchievementMetric.LongestGameMinutes, 600, 60, hidden = true),
        EmuAchievementDefinition("god_of_war_secret", R.string.achievement_gow_chains_title, R.string.achievement_gow_chains_description, EmuAchievementMetric.GodOfWarMinutes, 120, 50, hidden = true),
        EmuAchievementDefinition("liberty_city_secret", R.string.achievement_gta_lcs_title, R.string.achievement_gta_lcs_description, EmuAchievementMetric.GtaLcsMinutes, 120, 50, hidden = true),
        EmuAchievementDefinition("tekken_dr_secret", R.string.achievement_tekken_dr_title, R.string.achievement_tekken_dr_description, EmuAchievementMetric.TekkenDrMinutes, 90, 50, hidden = true)
        ))

        fun milestones(prefix: String, metric: EmuAchievementMetric, targets: List<Long>, title: Int, description: Int, hidden: Boolean = false) {
            targets.forEachIndexed { index, target ->
                add(EmuAchievementDefinition(
                    id = "${prefix}_$target",
                    titleRes = title,
                    descriptionRes = description,
                    metric = metric,
                    target = target,
                    points = (10 + index * 5).coerceAtMost(100),
                    hidden = hidden,
                    templatedText = true
                ))
            }
        }

        milestones("library", EmuAchievementMetric.GamesPlayed, listOf(2, 3, 10, 15, 30, 40, 50, 75, 100), R.string.achievement_library_title, R.string.achievement_library_description)
        milestones("total_minutes", EmuAchievementMetric.TotalPlayTimeMinutes, listOf(30, 120, 300, 1_200, 1_800, 3_000, 4_500, 9_000, 15_000, 30_000), R.string.achievement_total_time_title, R.string.achievement_total_time_description)
        milestones("sessions", EmuAchievementMetric.TotalSessions, listOf(1, 5, 25, 50, 100, 250, 500, 1_000), R.string.achievement_sessions_title, R.string.achievement_sessions_description)
        milestones("single_game_minutes", EmuAchievementMetric.LongestGameMinutes, listOf(30, 60, 120, 300, 1_200, 3_000, 6_000), R.string.achievement_single_game_time_title, R.string.achievement_single_game_time_description, hidden = true)
        milestones("single_game_sessions", EmuAchievementMetric.MaxGameSessions, listOf(2, 5, 10, 25, 50, 100), R.string.achievement_single_game_sessions_title, R.string.achievement_single_game_sessions_description)
        milestones("ps1_library", EmuAchievementMetric.PspGames, listOf(2, 3, 5, 10, 20), R.string.achievement_psp_library_title, R.string.achievement_psp_library_description)
        milestones("ps1_minutes", EmuAchievementMetric.PspMinutes, listOf(30, 60, 180, 300, 600, 1_500), R.string.achievement_psp_time_title, R.string.achievement_psp_time_description)
        milestones("ps1_sessions", EmuAchievementMetric.PspSessions, listOf(1, 5, 10, 25, 50), R.string.achievement_psp_sessions_title, R.string.achievement_psp_sessions_description)

        val topGames = listOf(
            "God of War: Chains of Olympus" to "chains of olympus",
            "Grand Theft Auto: Liberty City Stories" to "liberty city stories",
            "Tekken: Dark Resurrection" to "dark resurrection",
            "Crisis Core: Final Fantasy VII" to "crisis core",
            "Monster Hunter Freedom Unite" to "monster hunter",
            "Gran Turismo" to "gran turismo",
            "Metal Gear Solid: Peace Walker" to "peace walker",
            "Final Fantasy VII" to "final fantasy vii",
            "Persona 3 Portable" to "persona 3",
            "Kingdom Hearts: Birth by Sleep" to "birth by sleep",
            "Daxter" to "daxter",
            "Ratchet & Clank: Size Matters" to "size matters",
            "Silent Hill" to "silent hill",
            "Wipeout" to "wipeout"
        )
        topGames.forEachIndexed { index, (game, term) ->
            val slug = term.replace(Regex("[^a-z0-9]+"), "_").trim('_')
            add(EmuAchievementDefinition("game_${slug}_time", R.string.achievement_game_time_title, R.string.achievement_game_time_description, EmuAchievementMetric.MatchingTitleMinutes, 120, 50 + index, hidden = true, textArgument = game, matchTerms = listOf(term), templatedText = true))
            add(EmuAchievementDefinition("game_${slug}_sessions", R.string.achievement_game_sessions_title, R.string.achievement_game_sessions_description, EmuAchievementMetric.MatchingTitleSessions, 10, 45 + index, hidden = true, textArgument = game, matchTerms = listOf(term), templatedText = true))
        }

        listOf(
            "God of War" to "god of war", "Grand Theft Auto" to "grand theft auto",
            "Tekken" to "tekken", "Final Fantasy" to "final fantasy",
            "Metal Gear" to "metal gear", "Monster Hunter" to "monster hunter",
            "Gran Turismo" to "gran turismo", "Silent Hill" to "silent hill"
        ).forEach { (series, term) ->
            val slug = term.replace(' ', '_')
            add(EmuAchievementDefinition("series_$slug", R.string.achievement_series_title, R.string.achievement_series_description, EmuAchievementMetric.MatchingGamesCount, 2, 60, hidden = true, textArgument = series, matchTerms = listOf(term), templatedText = true))
        }
    }.also { definitions ->
        check(definitions.size >= 100)
        check(definitions.map { it.id }.distinct().size == definitions.size)
    }
}

class EmuAchievementRepository(context: Context) {
    private val appContext = context.applicationContext
    private val auth = FirebaseAuth.getInstance()
    private val firestore = FirebaseFirestore.getInstance()

    suspend fun evaluate(profile: PlayerProfile): List<EmuAchievementState> {
        if (auth.currentUser?.uid != profile.uid) return loadPublic(profile.uid, profile)
        return evaluateAndSync(profile.toAchievementSnapshot())
    }

    suspend fun evaluateCurrentProfile(): List<EmuAchievementState> {
        val uid = auth.currentUser?.uid ?: return emptyList()
        val profile = firestore.collection(USERS).document(uid).get().await().toAchievementSnapshot()
            ?: return emptyList()
        return evaluateAndSync(profile)
    }

    private suspend fun evaluateAndSync(snapshot: AchievementSnapshot): List<EmuAchievementState> {
        val uid = auth.currentUser?.uid ?: return emptyList()
        val unlockDocuments = runCatching {
            firestore.collection(UNLOCKS).whereEqualTo("uid", uid).get().await().documents
        }.onFailure { Log.w(TAG, "Unable to load achievement unlocks; using local progress", it) }
            .getOrDefault(emptyList())
        val unlocks = unlockDocuments.associate { it.getString("achievementId").orEmpty() to it.unlockedAtMs() }
        val candidates = EmuCoreAAchievementCatalog.definitions.filter { definition ->
            unlocks[definition.id] == null && snapshot.progressFor(definition) >= definition.target
        }

        val newlyUnlocked = if (candidates.isEmpty()) emptyList() else runCatching { firestore.runTransaction { transaction ->
            // Firestore transactions require every read to happen before the first write.
            val unreadUnlocks = candidates.map { definition ->
                val unlockReference = firestore.collection(UNLOCKS).document("${uid}_${definition.id}")
                val eventReference = firestore.collection(PUBLIC_PROFILES).document(uid)
                    .collection(FEED).document("achievement_${definition.id}")
                AchievementSyncCandidate(
                    definition = definition,
                    unlockReference = unlockReference,
                    unlockExists = transaction.get(unlockReference).exists(),
                    eventReference = eventReference,
                    eventExists = transaction.get(eventReference).exists()
                )
            }
            unreadUnlocks.filterNot(AchievementSyncCandidate::unlockExists).map { candidate ->
                val definition = candidate.definition
                val progress = snapshot.progressFor(definition)
                transaction.set(candidate.unlockReference, mapOf(
                    "uid" to uid,
                    "achievementId" to definition.id,
                    "unlockedAt" to FieldValue.serverTimestamp(),
                    "progress" to progress.coerceAtMost(Int.MAX_VALUE.toLong()).toInt(),
                    "target" to definition.target.coerceAtMost(Int.MAX_VALUE.toLong()).toInt(),
                    "catalogVersion" to EmuCoreAAchievementCatalog.VERSION,
                    "sourceGameSerial" to snapshot.lastSerial.orEmpty().take(32),
                    "visibility" to "public"
                ))
                if (!candidate.eventExists) {
                    val eventId = "achievement_${definition.id}"
                    transaction.set(candidate.eventReference, mapOf(
                        "uid" to uid,
                        "eventId" to eventId,
                        "type" to "achievement_unlocked",
                        "achievementId" to definition.id,
                        "visibility" to "public",
                        "createdAt" to FieldValue.serverTimestamp()
                    ))
                }
                definition
            }
        }.await() }.onFailure { Log.w(TAG, "Unable to sync achievement unlocks", it) }.getOrDefault(emptyList())

        if (newlyUnlocked.size <= 3) {
            newlyUnlocked.forEach(::notifyUnlocked)
        } else {
            notifyBulkUnlocked(newlyUnlocked.size)
        }
        val now = System.currentTimeMillis()
        val states = EmuCoreAAchievementCatalog.definitions.map { definition ->
            // Progress is the local source of truth. A temporary Firestore failure must not
            // make a completed achievement look locked or discard its points in the UI.
            resolveAchievementState(
                definition = definition,
                progress = snapshot.progressFor(definition),
                cloudUnlockedAtMs = unlocks[definition.id],
                localCompletedAtMs = now
            )
        }
        syncPublicSummary(uid, states)
        return states
    }

    private suspend fun syncPublicSummary(uid: String, states: List<EmuAchievementState>) {
        val unlocked = states.filter(EmuAchievementState::unlocked)
        runCatching {
            firestore.collection(PUBLIC_PROFILES).document(uid).set(
                mapOf(
                    "uid" to uid,
                    "achievementCount" to unlocked.size,
                    "achievementPoints" to unlocked.sumOf { it.definition.points },
                    "achievementCatalogVersion" to EmuCoreAAchievementCatalog.VERSION,
                    "updatedAt" to FieldValue.serverTimestamp()
                ),
                SetOptions.merge()
            ).await()
        }.onFailure { Log.w(TAG, "Unable to sync public achievement summary", it) }
    }

    private suspend fun loadPublic(uid: String, profile: PlayerProfile): List<EmuAchievementState> {
        val unlocks = firestore.collection(UNLOCKS)
            .whereEqualTo("uid", uid)
            .whereEqualTo("visibility", "public")
            .get().await().documents
            .associate { it.getString("achievementId").orEmpty() to it.unlockedAtMs() }
        val snapshot = profile.toAchievementSnapshot()
        return EmuCoreAAchievementCatalog.definitions.map { definition ->
            EmuAchievementState(
                definition = definition,
                progress = if (definition.id in unlocks) definition.target else 0,
                unlockedAtMs = unlocks[definition.id]
            )
        }
    }

    private fun notifyUnlocked(definition: EmuAchievementDefinition) {
        notifyAchievement(
            text = if (!definition.templatedText) appContext.getString(definition.titleRes)
            else if (definition.textArgument != null) appContext.getString(definition.titleRes, definition.textArgument)
            else appContext.getString(definition.titleRes, definition.target),
            notificationId = definition.id.hashCode()
        )
    }

    private fun notifyBulkUnlocked(count: Int) {
        notifyAchievement(appContext.getString(R.string.achievement_bulk_unlocked, count), BULK_NOTIFICATION_ID)
    }

    private fun notifyAchievement(text: String, notificationId: Int) {
        val manager = appContext.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            manager.createNotificationChannel(NotificationChannel(
                CHANNEL_ID,
                appContext.getString(R.string.achievement_notification_channel),
                NotificationManager.IMPORTANCE_DEFAULT
            ))
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            ContextCompat.checkSelfPermission(appContext, Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) return
        val notification = NotificationCompat.Builder(appContext, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_launcher_foreground)
            .setContentTitle(appContext.getString(R.string.achievement_unlocked_notification))
            .setContentText(text)
            .setAutoCancel(true)
            .setPriority(NotificationCompat.PRIORITY_DEFAULT)
            .build()
        NotificationManagerCompat.from(appContext).notify(notificationId, notification)
    }

    private data class AchievementSnapshot(
        val gamesPlayed: Long,
        val totalPlayTimeMs: Long,
        val games: List<AchievementGame>,
        val lastSerial: String?
    ) {
        fun progressFor(definition: EmuAchievementDefinition): Long = when (definition.metric) {
            EmuAchievementMetric.GamesPlayed -> gamesPlayed
            EmuAchievementMetric.TotalPlayTimeMinutes -> totalPlayTimeMs / 60_000L
            EmuAchievementMetric.TotalSessions -> games.sumOf { it.sessions }
            EmuAchievementMetric.PspGames -> pspGames().size.toLong()
            EmuAchievementMetric.PspMinutes -> pspGames().sumOf { it.totalPlayTimeMs } / 60_000L
            EmuAchievementMetric.PspSessions -> pspGames().sumOf { it.sessions }
            EmuAchievementMetric.LongestGameMinutes -> (games.maxOfOrNull { it.totalPlayTimeMs } ?: 0L) / 60_000L
            EmuAchievementMetric.MaxGameSessions -> games.maxOfOrNull { it.sessions } ?: 0L
            EmuAchievementMetric.MatchingTitleMinutes -> matching(definition).sumOf { it.totalPlayTimeMs } / 60_000L
            EmuAchievementMetric.MatchingTitleSessions -> matching(definition).sumOf { it.sessions }
            EmuAchievementMetric.MatchingGamesCount -> matching(definition).size.toLong()
            EmuAchievementMetric.GodOfWarMinutes -> minutesForTitle("god of war")
            EmuAchievementMetric.GtaLcsMinutes -> minutesForTitle("liberty city")
            EmuAchievementMetric.TekkenDrMinutes -> minutesForTitle("tekken")
        }

        // PSP disc serials use ULUS/ULES/ULJM/UCUS/UCES prefixes, while
        // PSN releases use NPJH/NPUG style prefixes.
        private fun pspGames() = games.filter { game ->
            val serial = game.serial.orEmpty().uppercase(Locale.ROOT)
            serial.startsWith("UL") || serial.startsWith("UC") || serial.startsWith("NP")
        }

        private fun matching(definition: EmuAchievementDefinition): List<AchievementGame> = games.filter { game ->
            val title = game.title.lowercase(Locale.ROOT)
            definition.matchTerms.any { term -> title.contains(term.lowercase(Locale.ROOT)) }
        }

        private fun minutesForTitle(needle: String): Long = games
            .filter { it.title.lowercase(Locale.ROOT).contains(needle) }
            .sumOf { it.totalPlayTimeMs } / 60_000L
    }

    private data class AchievementGame(
        val title: String,
        val serial: String?,
        val totalPlayTimeMs: Long,
        val sessions: Long
    )

    private data class AchievementSyncCandidate(
        val definition: EmuAchievementDefinition,
        val unlockReference: com.google.firebase.firestore.DocumentReference,
        val unlockExists: Boolean,
        val eventReference: com.google.firebase.firestore.DocumentReference,
        val eventExists: Boolean
    )

    private fun PlayerProfile.toAchievementSnapshot() = AchievementSnapshot(
        gamesPlayed = gamesPlayed.toLong(),
        totalPlayTimeMs = totalPlayTimeMs,
        games = games.map { AchievementGame(it.title, it.serial, it.totalPlayTimeMs, it.sessions.toLong()) },
        lastSerial = games.maxByOrNull { it.lastPlayedAtMs ?: 0L }?.serial
    )

    private fun DocumentSnapshot.toAchievementSnapshot(): AchievementSnapshot? {
        if (!exists()) return null
        val rawGames = get("games") as? Map<*, *> ?: emptyMap<String, Any>()
        val games = rawGames.values.mapNotNull { raw ->
            val map = raw as? Map<*, *> ?: return@mapNotNull null
            AchievementGame(
                title = map["title"] as? String ?: return@mapNotNull null,
                serial = map["serial"] as? String,
                totalPlayTimeMs = (map["totalPlayTimeMs"] as? Number)?.toLong() ?: 0L,
                sessions = (map["sessions"] as? Number)?.toLong() ?: 0L
            )
        }
        return AchievementSnapshot(
            gamesPlayed = getLong("gamesPlayed") ?: games.size.toLong(),
            totalPlayTimeMs = getLong("totalPlayTimeMs") ?: games.sumOf { it.totalPlayTimeMs },
            games = games,
            lastSerial = games.maxByOrNull { it.totalPlayTimeMs }?.serial
        )
    }

    private fun DocumentSnapshot.unlockedAtMs(): Long = getTimestamp("unlockedAt")?.toDate()?.time ?: 1L

    private suspend fun <T> Task<T>.await(): T = suspendCancellableCoroutine { continuation ->
        addOnSuccessListener { continuation.resume(it) }
        addOnFailureListener { continuation.resumeWithException(it) }
        addOnCanceledListener { continuation.cancel() }
    }

    private companion object {
        const val USERS = "users"
        const val PUBLIC_PROFILES = "publicProfiles"
        const val UNLOCKS = "achievementUnlocks"
        const val FEED = "feed"
        const val CHANNEL_ID = "emucorea_achievements"
        const val TAG = "EmuAchievements"
        const val BULK_NOTIFICATION_ID = 0x454D55
    }
}
