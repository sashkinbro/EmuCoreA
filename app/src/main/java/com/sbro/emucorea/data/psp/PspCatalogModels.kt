package com.sbro.emucorea.data.psp

data class PspCatalogSummary(
    val igdbId: Long,
    val name: String,
    val normalizedName: String,
    val storyline: String?,
    val summary: String?,
    val year: Int?,
    val rating: Double?,
    val coverUrl: String?,
    val heroUrl: String?,
    val genres: List<String> = emptyList(),
    val primarySerial: String? = null
)

data class PspCatalogDetails(
    val igdbId: Long,
    val name: String,
    val normalizedName: String,
    val year: Int?,
    val rating: Double?,
    val storyline: String?,
    val summary: String?,
    val genres: List<String>,
    val screenshots: List<String>,
    val videos: List<String>,
    val coverUrl: String?,
    val heroUrl: String?,
    val primarySerial: String? = null
)
