package com.hr54.controller

import com.hr54.controller.data.api.*
import com.hr54.controller.data.model.*
import kotlinx.serialization.json.*
import org.junit.Assert.*
import org.junit.Test

class ContractTest {
    @Test
    fun bareAddress() {
        assertEquals("http://192.168.88.103:8130", normalizeReceiver(" 192.168.88.103 "))
    }

    @Test
    fun fullAddress() {
        assertEquals("http://192.168.88.103:8130", normalizeReceiver("http://192.168.88.103:8130/"))
    }

    @Test
    fun customAndHttpsPorts() {
        assertEquals("https://receiver:9443", normalizeReceiver("https://receiver:9443"))
        assertEquals("http://receiver:8130", normalizeReceiver("receiver"))
        assertEquals("http://receiver:80", normalizeReceiver("http://receiver:80"))
    }

    @Test(expected = IllegalArgumentException::class)
    fun rejectPath() {
        normalizeReceiver("http://receiver/api")
    }

    @Test(expected = IllegalArgumentException::class)
    fun rejectCredentials() {
        normalizeReceiver("http://user:secret@receiver")
    }

    @Test
    fun capabilities() {
        val caps =
            apiJson.decodeFromString<Capabilities>(
                """{"ok":true,"jellyfin":true,"youtube":false,"doom":false}"""
            )
        assertEquals(listOf("Home", "Jellyfin"), caps.destinations())
    }

    @Test
    fun parseStateAndTransport() {
        val p =
            apiJson.decodeFromString<Playback>(
                """{"playing":true,"source":"iptv","duration":null,"transport":{"stop":true,"seek":false}}"""
            )
        assertTrue(p.transport.stop)
        assertFalse(p.transport.pause)
        assertFalse(p.transport.seek)
        assertNull(p.duration)
    }

    @Test
    fun parseRuntimeTicks() {
        val item =
            apiJson.decodeFromString<Item>(
                """{"id":"a","name":"Alien","runtime":70200000000,"year":1979,"childCount":null}"""
            )
        assertEquals(7020.0, item.seconds!!, 0.0)
    }

    @Test
    fun parsePaging() {
        val channels =
            apiJson.decodeFromString<Channels>(
                """{"channels":[],"total":121,"offset":60,"limit":60,"hasMore":true}"""
            )
        assertTrue(channels.hasMore)
        assertEquals(60, channels.offset)
        val videos = apiJson.decodeFromString<Videos>("""{"page":2,"hasMore":false,"results":[]}""")
        assertEquals(2, videos.page)
        assertFalse(videos.hasMore)
    }

    @Test
    fun truncateAscii() {
        assertEquals("a".repeat(64), truncateUtf8("a".repeat(80)))
    }

    @Test
    fun truncateUnicode() {
        assertEquals("😀".repeat(16), truncateUtf8("😀".repeat(20)))
        assertEquals("a".repeat(63), truncateUtf8("a".repeat(63) + "é"))
        assertEquals("界".repeat(21), truncateUtf8("界".repeat(30)))
    }

    @Test
    fun unicodeNeverBroken() {
        val source = "é界😀".repeat(20)
        for (n in 0..64) {
            val value = truncateUtf8(source, n)
            assertTrue(value.toByteArray(Charsets.UTF_8).size <= n)
            assertFalse(value.lastOrNull()?.isHighSurrogate() ?: false)
            assertEquals(value, String(value.toByteArray(Charsets.UTF_8), Charsets.UTF_8))
        }
    }

    @Test
    fun insertionAndSelection() {
        assertEquals(
            InsertedText("find Alien tonight", 10),
            insertTranscript("find  tonight", 5, 5, "Alien"),
        )
        assertEquals(InsertedText("Alien", 5), insertTranscript("Dune", 0, 4, "Alien"))
        assertEquals(InsertedText("hello", 5), insertTranscript("", 0, 0, " hello "))
    }

    @Test
    fun insertionTruncates() {
        val r = insertTranscript("a".repeat(63), 63, 63, "😀")
        assertEquals(63, r.cursor)
        assertEquals(63, r.text.length)
    }

    @Test
    fun thumbnailsValidateIds() {
        assertNull(Video("../../foo", "title").thumbnail)
        assertEquals(
            "https://i.ytimg.com/vi/jNQXAC9IVRw/hqdefault.jpg",
            Video("jNQXAC9IVRw", "title").thumbnail,
        )
    }

    @Test
    fun insertionDoesNotSplitSurrogates() {
        assertEquals(InsertedText("hi😀", 2), insertTranscript("😀", 1, 1, "hi"))
    }

    @Test
    fun normalizationIsIdempotentAndSupportsIpv6() {
        for (input in
            listOf("receiver", "http://receiver:80", "https://receiver:443", "[fd00::1]")) {
            val normalized = normalizeReceiver(input)
            assertEquals(normalized, normalizeReceiver(normalized))
        }
        assertEquals("http://[fd00::1]:8130", normalizeReceiver("[fd00::1]"))
    }
}
