package com.hr54.controller

import com.hr54.controller.data.api.*
import kotlinx.coroutines.test.runTest
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.junit.Assert.*
import org.junit.Test

class HttpApiTest {
    @Test
    fun seekAllowsSlowReceiverStreamPreparation() = runTest {
        val server = MockWebServer()
        server.start()
        try {
            server.enqueue(MockResponse().setHeadersDelay(16, java.util.concurrent.TimeUnit.SECONDS)
                .setBody("""{"ok":true,"playing":true}"""))
            val response = HttpReceiverApi(server.url("/").toString().trimEnd('/'))
                .post("/api/playback/seek", body("seconds" to 120))
            assertTrue(response.toString().contains("\"playing\":true"))
        } finally {
            server.shutdown()
        }
    }
    @Test
    fun missingDiscoveryExplainsServerUpgrade() = runTest {
        val server = MockWebServer()
        server.start()
        try {
            for (path in listOf("/api/capabilities", "/api/state")) {
                server.enqueue(MockResponse().setResponseCode(404).setBody("""{"error":"not found"}"""))
                try {
                    HttpReceiverApi(server.url("/").toString().trimEnd('/')).get(path)
                    fail("Missing discovery must prevent connection")
                } catch (e: ReceiverFailure) {
                    assertEquals(404, e.httpCode)
                    assertTrue(e.reason.contains("Receiver is reachable"))
                    assertTrue(e.reason.contains(path))
                    assertTrue(e.reason.contains("native-client API"))
                    assertTrue(e.reason.contains("not found"))
                }
            }
        } finally {
            server.shutdown()
        }
    }

    @Test
    fun preservesServerReason() = runTest {
        val server = MockWebServer()
        server.start()
        try {
            server.enqueue(
                MockResponse()
                    .setResponseCode(502)
                    .setBody("""{"ok":false,"error":"YouTube resolver failed"}""")
            )
            try {
                HttpReceiverApi(server.url("/").toString().trimEnd('/'))
                    .get("/api/youtube/search", mapOf("q" to "music", "page" to "0"))
                fail()
            } catch (e: ReceiverFailure) {
                assertEquals("YouTube resolver failed", e.reason)
                assertEquals(502, e.httpCode)
            }
            assertEquals("/api/youtube/search?q=music&page=0", server.takeRequest().path)
        } finally {
            server.shutdown()
        }
    }

    @Test
    fun postJson() = runTest {
        val server = MockWebServer()
        server.start()
        try {
            server.enqueue(MockResponse().setBody("{\"ok\":true}"))
            HttpReceiverApi(server.url("/").toString().trimEnd('/'))
                .post("/api/play", body("itemId" to "a", "returnToTv" to true))
            val request = server.takeRequest()
            assertEquals("POST", request.method)
            assertTrue(request.body.readUtf8().contains("\"returnToTv\":true"))
        } finally {
            server.shutdown()
        }
    }
}
