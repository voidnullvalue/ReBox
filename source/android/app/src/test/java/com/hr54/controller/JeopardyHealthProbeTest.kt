package com.hr54.controller

import com.hr54.controller.game.JeopardyHealth
import com.hr54.controller.game.JeopardyHealthProbe
import com.hr54.controller.game.JeopardyWebPolicy
import kotlinx.coroutines.runBlocking
import okhttp3.OkHttpClient
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import okhttp3.mockwebserver.SocketPolicy
import org.junit.After
import org.junit.Assert.*
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test
import java.util.concurrent.TimeUnit

/**
 * Deterministic host coverage for the bounded LAN game health probe. It binds the
 * real game port so it can assert the probe only ever issues a single HTTP GET to
 * `/health` on the configured game origin (port 8135).
 */
class JeopardyHealthProbeTest {
    private var server: MockWebServer? = null
    private val gameUrl = "http://127.0.0.1:${JeopardyWebPolicy.GAME_PORT}/"

    @Before fun start() {
        // Each test reuses the fixed game port with a fresh server; drop any pooled
        // socket from the previous server so the probe always sees the new one.
        JeopardyHealthProbe.client.connectionPool.evictAll()
        val s = MockWebServer()
        try {
            s.start(JeopardyWebPolicy.GAME_PORT)
            server = s
        } catch (e: java.io.IOException) {
            s.shutdown()
            server = null
        }
    }

    @After fun stop() {
        server?.shutdown()
        JeopardyHealthProbe.client.connectionPool.evictAll()
    }

    private fun runningServer(): MockWebServer {
        val s = server
        assumeTrue("port ${JeopardyWebPolicy.GAME_PORT} must be free for this test", s != null && s.port == JeopardyWebPolicy.GAME_PORT)
        return s!!
    }

    @Test fun healthUrlIsOnlyForThePlainHttpGameOriginOn8135() {
        assertEquals("http://192.168.88.103:8135/health", JeopardyHealthProbe.healthUrl(JeopardyWebPolicy.gameUrl("http://192.168.88.103:8130")))
        assertNull(JeopardyHealthProbe.healthUrl("https://192.168.88.103:8135/"))
        assertNull(JeopardyHealthProbe.healthUrl("http://192.168.88.103:8130/"))
        assertNull(JeopardyHealthProbe.healthUrl("http://user:pass@192.168.88.103:8135/"))
    }

    @Test fun productionClientIsBoundedAndNeverFollowsRedirects() {
        assertFalse(JeopardyHealthProbe.client.followRedirects)
        assertFalse(JeopardyHealthProbe.client.followSslRedirects)
        assertEquals(4000, JeopardyHealthProbe.client.callTimeoutMillis)
    }

    @Test fun classifyRequiresTheLocalGameIdentity() {
        assertEquals(JeopardyHealth.Healthy, JeopardyHealthProbe.classify(200, """{"ok":true,"service":"jeopardy","capacity":6}"""))
        assertTrue(JeopardyHealthProbe.classify(200, "<html><body>portal</body></html>") is JeopardyHealth.Unhealthy)
        assertTrue(JeopardyHealthProbe.classify(200, """{"ok":true,"service":"portal"}""") is JeopardyHealth.Unhealthy)
        assertTrue(JeopardyHealthProbe.classify(404, """{"ok":false}""") is JeopardyHealth.Unhealthy)
    }

    @Test fun classifyNeverThrowsOnNonPrimitiveIdentityFields() {
        // A hostile/portal body may put an object/array where the identity scalar
        // is expected; classify must report Unhealthy rather than throw.
        val bodies = listOf(
            """{"ok":{"x":1}}""",
            """{"ok":[1,2],"service":"jeopardy"}""",
            """{"ok":true,"service":{"name":"jeopardy"}}""",
            """{"ok":true,"service":["jeopardy"]}""",
            "{",
            "",
        )
        for (body in bodies) {
            assertTrue(
                "classify($body) must be Unhealthy",
                runCatching { JeopardyHealthProbe.classify(200, body) }.getOrNull() is JeopardyHealth.Unhealthy,
            )
        }
    }

    @Test fun healthyGameIsAcceptedAndOnlyHealthIsRequestedOn8135() = runBlocking {
        val s = runningServer()
        s.enqueue(MockResponse().setResponseCode(200).setBody("""{"ok":true,"service":"jeopardy","capacity":6}"""))
        assertEquals(JeopardyHealth.Healthy, JeopardyHealthProbe.probe(gameUrl))
        val request = s.takeRequest()
        assertEquals("/health", request.path)
        assertEquals("GET", request.method)
        assertNull("no management token is ever sent", request.getHeader("Authorization"))
        assertEquals(8135, request.requestUrl!!.port)
        assertEquals(1, s.requestCount)
    }

    @Test fun non200IsUnhealthy() = runBlocking {
        val s = runningServer()
        s.enqueue(MockResponse().setResponseCode(503).setBody("down"))
        val health = JeopardyHealthProbe.probe(gameUrl)
        assertTrue(health is JeopardyHealth.Unhealthy)
        assertTrue((health as JeopardyHealth.Unhealthy).reason.contains("503"))
    }

    @Test fun htmlCaptivePortalIsNotTheGame() = runBlocking {
        val s = runningServer()
        s.enqueue(MockResponse().setResponseCode(200).setBody("<html><body>Sign in to WiFi</body></html>"))
        assertTrue(JeopardyHealthProbe.probe(gameUrl) is JeopardyHealth.Unhealthy)
    }

    @Test fun redirectIsNeverFollowedAndTheExternalHostIsNeverReached() = runBlocking {
        val s = runningServer()
        s.enqueue(MockResponse().setResponseCode(302).setHeader("Location", "https://evil.test/game"))
        val health = JeopardyHealthProbe.probe(gameUrl)
        assertTrue(health is JeopardyHealth.Unhealthy)
        assertTrue((health as JeopardyHealth.Unhealthy).reason.contains("redirected"))
        assertEquals("the external Location was never contacted", 1, s.requestCount)
    }

    @Test fun stalledGameTimesOutIntoAReadableFailure() = runBlocking {
        val s = runningServer()
        s.enqueue(MockResponse().setSocketPolicy(SocketPolicy.NO_RESPONSE))
        val fast = OkHttpClient.Builder()
            .followRedirects(false)
            .connectTimeout(300, TimeUnit.MILLISECONDS)
            .readTimeout(300, TimeUnit.MILLISECONDS)
            .callTimeout(800, TimeUnit.MILLISECONDS)
            .build()
        assertTrue(JeopardyHealthProbe.probe(gameUrl, fast) is JeopardyHealth.Unhealthy)
    }

    @Test fun unsupportedAddressIsRejectedWithoutAnyRequest() = runBlocking {
        assertTrue(JeopardyHealthProbe.probe("https://192.168.88.103:8135/") is JeopardyHealth.Unhealthy)
        assertTrue(JeopardyHealthProbe.probe("http://192.168.88.103:8130/") is JeopardyHealth.Unhealthy)
    }
}
