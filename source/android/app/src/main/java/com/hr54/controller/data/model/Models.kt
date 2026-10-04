package com.hr54.controller.data.model

import kotlinx.serialization.Serializable

@Serializable
data class Capabilities(
    val jellyfin: Boolean = false,
    val iptv: Boolean = false,
    val youtube: Boolean = false,
    val frigate: Boolean = false,
    val playback: Boolean = false,
) {
    fun destinations() =
        listOf("Home") +
            listOfNotNull(
                "Jellyfin".takeIf { jellyfin },
                "Live TV".takeIf { iptv },
                "YouTube".takeIf { youtube },
                "Cameras".takeIf { frigate },
            )
}

@Serializable
data class Transport(
    val stop: Boolean = false,
    val pause: Boolean = false,
    val resume: Boolean = false,
    val seek: Boolean = false,
)

@Serializable
data class Playback(
    val playing: Boolean = false,
    val source: String? = null,
    val name: String = "",
    val title: String = "",
    val itemId: String? = null,
    val paused: Boolean = false,
    val live: Boolean = false,
    val elapsed: Double = 0.0,
    val duration: Double? = null,
    val transport: Transport = Transport(),
)

@Serializable
data class Auth(
    val authenticated: Boolean = false,
    val pending: Boolean = false,
    val code: String? = null,
    val expired: Boolean = false,
)

@Serializable
data class Item(
    val id: String,
    val name: String,
    val type: String = "",
    val year: Int? = null,
    val runtime: Long? = null,
    val overview: String = "",
    val playable: Boolean = false,
    val isFolder: Boolean = false,
    val childCount: Int? = null,
) {
    val seconds: Double?
        get() = runtime?.div(10_000_000.0)
}

@Serializable data class Items(val items: List<Item> = emptyList(), val total: Int = 0)

@Serializable data class Libraries(val libraries: List<Item> = emptyList())

@Serializable data class Group(val name: String, val count: Int = 0)

@Serializable data class Groups(val groups: List<Group> = emptyList())

@Serializable
data class Channel(
    val id: String,
    val name: String,
    val tvgId: String = "",
    val group: String = "",
    val logo: String = "",
)

@Serializable
data class Channels(
    val channels: List<Channel> = emptyList(),
    val total: Int = 0,
    val offset: Int = 0,
    val limit: Int = 60,
    val hasMore: Boolean = false,
)

@Serializable
data class Video(
    val id: String,
    val title: String,
    val channel: String = "",
    val duration: Double? = null,
) {
    val thumbnail: String?
        get() =
            if (Regex("[A-Za-z0-9_-]{11}").matches(id)) "https://i.ytimg.com/vi/$id/hqdefault.jpg"
            else null
}

@Serializable
data class Videos(
    val results: List<Video> = emptyList(),
    val page: Int = 0,
    val hasMore: Boolean = false,
)

@Serializable
data class Camera(
    val id: String,
    val name: String,
    val stream: String = "",
    val playable: Boolean = false,
    val live: Boolean = true,
    val reason: String = "",
)

@Serializable data class Cameras(val cameras: List<Camera> = emptyList())

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
