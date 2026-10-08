package com.hr54.controller.data.model

import kotlinx.serialization.Serializable
import kotlinx.serialization.json.*
import okhttp3.HttpUrl.Companion.toHttpUrl

fun moduleId(id: String): Boolean = Regex("[a-z0-9][a-z0-9._-]{0,62}").matches(id)
fun moduleRoute(id: String, operation: String = ""): String {
    require(moduleId(id)) { "Invalid module ID" }
    return "/api/modules/$id" + if (operation.isEmpty()) "" else "/$operation"
}
fun moduleAsset(base: String?, id: String, artwork: String? = null): String? = base?.let {
    require(moduleId(id))
    it.toHttpUrl().newBuilder().addPathSegments("api/modules").addPathSegment(id).apply {
        if (artwork.isNullOrEmpty()) addPathSegment("icon")
        else { addPathSegment("art"); addPathSegment(artwork) }
    }.build().toString()
}
@Serializable data class ModuleCapabilities(val browse: Boolean = false, val search: Boolean = false,
    val playback: Boolean = false, val settings: Boolean = false, val auth: Boolean = false, val nativeApp: Boolean = false)
@Serializable data class Presentation(val releaseInput: Boolean = false, val releaseSurface: Boolean = false)
@Serializable data class ModuleDescriptor(val id: String, val name: String, val version: String = "",
    val description: String = "", val kind: String = "media", val core: Boolean = false,
    val installed: Boolean = false, val enabled: Boolean = false, val healthy: Boolean = false,
    val compatible: Boolean = true, val home: Boolean = true, val order: Int = 0, val error: String = "",
    val capabilities: ModuleCapabilities = ModuleCapabilities(), val presentation: Presentation = Presentation())
@Serializable data class ModuleRegistry(val moduleApi: Int, val modules: List<ModuleDescriptor> = emptyList()) {
    fun validated(): ModuleRegistry {
        require(moduleApi == 1 && modules.size <= 32 && modules.map { it.id }.distinct().size == modules.size) { "Unsupported or oversized module registry" }
        modules.forEach { require(moduleId(it.id) && it.name.isNotBlank() && it.name.toByteArray().size <= 127 &&
            it.version.toByteArray().size <= 63 && it.description.toByteArray().size <= 255 &&
            it.kind in listOf("media", "native-app")) { "Invalid module descriptor" } }
        return this
    }
}
@Serializable data class MediaItem(val id: String, val title: String, val kind: String = "item",
    val playable: Boolean = false, val subtitle: String = "", val description: String = "", val artwork: String = "",
    val year: Int = 0, val duration: Double = 0.0, val resume: Double = 0.0)
@Serializable data class MediaPage(val items: List<MediaItem> = emptyList(), val total: Int = 0,
    val offset: Int = 0, val hasMore: Boolean = false) {
    fun validated(): MediaPage {
        require(items.size <= 60 && total >= 0 && offset >= 0 && (!hasMore || items.isNotEmpty())) { "Invalid media page" }
        items.forEach { require(it.id.isNotEmpty() && it.id.toByteArray().size <= 255 && it.title.isNotBlank() &&
            it.title.toByteArray().size <= 255 && it.artwork.toByteArray().size <= 255 &&
            it.description.toByteArray().size <= 1023 && it.kind in listOf("folder", "item", "action") &&
            it.duration.isFinite() && it.resume.isFinite() && it.duration >= 0 && it.resume >= 0) { "Invalid media item" } }
        return this
    }
}
@Serializable data class SettingChoice(val label: String, val value: JsonPrimitive)
@Serializable data class ModuleField(val key: String, val label: String, val type: String, val value: JsonPrimitive,
    val choices: List<SettingChoice> = emptyList())
@Serializable data class ModuleAction(val id: String, val label: String)
@Serializable data class ModuleSettings(val fields: List<ModuleField> = emptyList(), val actions: List<ModuleAction> = emptyList()) {
    fun validated(): ModuleSettings {
        require(fields.size <= 12 && actions.size <= 12 && fields.map { it.key }.distinct().size == fields.size) { "Oversized module settings" }
        fields.forEach { require(it.key.isNotBlank() && it.key.toByteArray().size <= 63 && it.label.toByteArray().size <= 127 &&
            it.type in listOf("bool", "integer", "string", "choice") && it.value.content.toByteArray().size <= 255 &&
            it.value != JsonNull && it.choices.all { choice -> choice.label.toByteArray().size <= 127 && choice.value != JsonNull && choice.value.content.toByteArray().size <= 255 } && it.choices.size <= 8 && (it.type != "choice" || it.choices.isNotEmpty()) &&
            (it.type != "bool" || (!it.value.isString && it.value.booleanOrNull != null)) && (it.type != "integer" || (!it.value.isString && it.value.longOrNull != null))) { "Invalid module setting" } }
        actions.forEach { require(moduleId(it.id) && it.label.toByteArray().size <= 127) { "Invalid module action" } }
        return this
    }
}
@Serializable data class ActionResult(val code: String? = null, val message: String = "", val pending: Boolean = false,
    val pollAction: String = "", val authenticated: Boolean = false) {
    fun validated(): ActionResult { require((code?.toByteArray()?.size ?: 0) <= 39 && message.toByteArray().size <= 255 &&
        (pollAction.isEmpty() || moduleId(pollAction))) { "Invalid module action result" }; return this }
}
@Serializable data class SystemStatus(val ready: Boolean = false, val mediaBusy: Boolean = true, val nativeModule: String = "")
@Serializable data class Transport(val stop: Boolean = false, val pause: Boolean = false, val resume: Boolean = false, val seek: Boolean = false, val channelUp: Boolean = false, val channelDown: Boolean = false)
@Serializable data class Playback(val playing: Boolean = false, val source: String? = null, val name: String = "", val title: String = "",
    val itemId: String? = null, val paused: Boolean = false, val live: Boolean = false, val elapsed: Double = 0.0,
    val duration: Double? = null, val transport: Transport = Transport(), val instance: String = "", val generation: Long = 0)

fun truncateUtf8(value: String, maxBytes: Int = 64): String {
    var index = 0
    var bytes = 0
    while (index < value.length) {
        val cp = value.codePointAt(index)
        if (cp in 0xD800..0xDFFF) break
        val size =
            when {
                cp <= 0x7F -> 1
                cp <= 0x7FF -> 2
                cp <= 0xFFFF -> 3
                else -> 4
            }
        if (bytes + size > maxBytes) break
        bytes += size
        index += Character.charCount(cp)
    }
    return value.substring(0, index)
}

data class InsertedText(val text: String, val cursor: Int)

fun insertTranscript(
    text: String,
    start: Int,
    end: Int,
    transcript: String,
    maxBytes: Int = 64,
): InsertedText {
    var a = minOf(start, end).coerceIn(0, text.length)
    var b = maxOf(start, end).coerceIn(a, text.length)
    fun splitsPair(index: Int) =
        index > 0 &&
            index < text.length &&
            text[index].isLowSurrogate() &&
            text[index - 1].isHighSurrogate()
    if (a == b && splitsPair(a)) {
        a--
        b--
    } else {
        if (splitsPair(a)) a--
        if (splitsPair(b)) b++
    }
    val inserted = transcript.trim()
    val result = truncateUtf8(text.substring(0, a) + inserted + text.substring(b), maxBytes)
    return InsertedText(result, minOf(a + inserted.length, result.length))
}

fun timeLabel(seconds: Double): String {
    val s = seconds.toLong().coerceAtLeast(0)
    return if (s >= 3600) "%d:%02d:%02d".format(s / 3600, s / 60 % 60, s % 60)
    else "%d:%02d".format(s / 60, s % 60)
}
