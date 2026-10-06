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
            for (path in listOf("/api/modules", "/api/state")) {
                server.enqueue(MockResponse().setResponseCode(404).setBody("""{"error":"not found"}"""))
                try {
                    HttpReceiverApi(server.url("/").toString().trimEnd('/')).get(path)
                    fail("Missing discovery must prevent connection")
                } catch (e: ReceiverFailure) {
                    assertEquals(404, e.httpCode)
                    assertTrue(e.reason.contains("Receiver is reachable"))
                    assertTrue(e.reason.contains(path))
                    assertTrue(e.reason.contains("runtime module API"))
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
                    .setBody("""{"ok":false,"error":"Module resolver failed"}""")
            )
            try {
                HttpReceiverApi(server.url("/").toString().trimEnd('/'))
                    .get("/api/modules/future-provider/search", mapOf("q" to "music", "offset" to "0"))
                fail()
            } catch (e: ReceiverFailure) {
                assertEquals("Module resolver failed", e.reason)
                assertEquals(502, e.httpCode)
            }
            assertEquals("/api/modules/future-provider/search?q=music&offset=0", server.takeRequest().path)
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

    @Test fun managementBearerOnlyGoesToProtectedRequestsAndDeleteIsSupported() = runTest {
        val server = MockWebServer(); server.start()
        try {
            val api = HttpReceiverApi(server.url("/").toString().trimEnd('/')); val token = "a".repeat(64); api.setManagementToken(token)
            for (path in listOf("/api/modules", "/api/state")) { server.enqueue(MockResponse().setBody("{\"ok\":true}")); api.get(path); assertNull(server.takeRequest().getHeader("Authorization")) }
            server.enqueue(MockResponse().setBody("{\"ok\":true}")); api.post("/api/management/pair", body("code" to "abcdef12")); assertNull(server.takeRequest().getHeader("Authorization"))
            for (path in listOf("/api/modules/install", "/api/modules/future-id/settings", "/api/modules/future-id/actions/connect", "/api/modules/future-id/enable")) {
                server.enqueue(MockResponse().setBody("{\"ok\":true}")); api.post(path); assertEquals("Bearer $token", server.takeRequest().getHeader("Authorization"))
            }
            server.enqueue(MockResponse().setBody("{\"ok\":true}")); api.delete("/api/modules/future-id"); val request = server.takeRequest(); assertEquals("DELETE", request.method); assertEquals("Bearer $token", request.getHeader("Authorization"))
        } finally { server.shutdown() }
    }
    @Test fun genericLongOperationsAndBoundedResponses() = runTest {
        assertTrue(longOperation("/api/modules/unknown/search")); assertTrue(longOperation("/api/modules/unknown/play")); assertTrue(longOperation("/api/modules/install")); assertFalse(longOperation("/api/state"))
        val server = MockWebServer(); server.start()
        try {
            server.enqueue(MockResponse().setBody("{\"data\":\"" + "x".repeat(2 * 1024 * 1024) + "\"}"))
            try { HttpReceiverApi(server.url("/").toString().trimEnd('/')).get("/api/modules"); fail() }
            catch (e: ReceiverFailure) { assertTrue(e.reason.contains("exceeds limit")) }
        } finally { server.shutdown() }
    }
    @Test fun authenticatedRedirectIsRefused() = runTest {
        val server = MockWebServer(); server.start()
        try {
            server.enqueue(MockResponse().setResponseCode(302).setHeader("Location", "http://other.invalid/api/modules/install").setBody("{\"error\":\"redirect refused\"}"))
            val api = HttpReceiverApi(server.url("/").toString().trimEnd('/')); api.setManagementToken("a".repeat(64))
            try { api.post("/api/modules/install"); fail() } catch (e: ReceiverFailure) { assertEquals(302, e.httpCode) }
            assertEquals(1, server.requestCount)
        } finally { server.shutdown() }
    }
}
