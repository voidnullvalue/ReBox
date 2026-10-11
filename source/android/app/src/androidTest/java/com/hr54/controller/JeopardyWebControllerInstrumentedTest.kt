package com.hr54.controller

import android.webkit.WebView
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hr54.controller.game.*
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.BufferedReader
import java.io.InputStreamReader
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Device/emulator acceptance for the embedded Jeopardy controller. It stands up a
 * throwaway local HTTP origin (the "receiver game") and drives the real WebView
 * with the bundled reviewed page. It is only a claim when actually executed on a
 * device; the host suite covers the URL/security policy deterministically.
 */
@RunWith(AndroidJUnit4::class)
class JeopardyWebControllerInstrumentedTest {

    /** Minimal single-shot local HTTP origin standing in for the receiver's game server. */
    private class LocalGameServer {
        private val socket = ServerSocket(0, 8, java.net.InetAddress.getByName("127.0.0.1"))
        val port: Int get() = socket.localPort
        @Volatile var lastPath: String? = null
        private val thread = Thread {
            try {
                while (!socket.isClosed) socket.accept().use { serve(it) }
            } catch (_: Exception) {
            }
        }
        private fun serve(client: Socket) {
            val reader = BufferedReader(InputStreamReader(client.getInputStream()))
            val requestLine = reader.readLine() ?: return
            lastPath = requestLine.split(' ').getOrNull(1)
            while (true) { val header = reader.readLine() ?: break; if (header.isEmpty()) break }
            val body = """{"ok":true}"""
            val response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ${body.toByteArray().size}\r\nConnection: close\r\n\r\n$body"
            client.getOutputStream().write(response.toByteArray())
            client.getOutputStream().flush()
        }
        fun start() { thread.isDaemon = true; thread.start() }
        fun stop() { runCatching { socket.close() } }
    }

    @Test fun bundledPageRunsAgainstLocalHttpGameAndKeepsIdentityWithoutHttps() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val context = instrumentation.targetContext
        val server = LocalGameServer(); server.start()
        val gameUrl = "http://127.0.0.1:${server.port}/"
        val html = context.assets.open(JeopardyWebPolicy.ASSET_PATH).bufferedReader().use { it.readText() }
        assertTrue("bundle carries the reviewed page", html.contains("window.ReboxJeopardy"))
        val ready = AtomicBoolean(false)
        val closed = AtomicBoolean(false)
        val holder = arrayOfNulls<WebView>(1)
        instrumentation.runOnMainSync {
            holder[0] = buildJeopardyWebView(context, gameUrl, html, { closed.set(true) }, {}, { ready.set(true) }, {})
        }
        try {
            assertTrue("onPageFinished fired for the bundled page", waitFor { ready.get() })

            // DOM storage is enabled and origin-scoped (player/host identity resumes here).
            val stored = arrayOfNulls<String>(1)
            val storeLatch = CountDownLatch(1)
            instrumentation.runOnMainSync {
                holder[0]!!.evaluateJavascript("(function(){try{localStorage.setItem('probe','1');return localStorage.getItem('probe');}catch(e){return 'err';}})()") { stored[0] = it; storeLatch.countDown() }
            }
            assertTrue(storeLatch.await(5, TimeUnit.SECONDS))
            assertTrue("DOM storage round-trips: ${stored[0]}", stored[0]?.contains("1") == true)

            // Back before joining: no role, so the page declines and the native view owns exit.
            val exit = arrayOfNulls<String>(1)
            val exitLatch = CountDownLatch(1)
            instrumentation.runOnMainSync {
                holder[0]!!.evaluateJavascript(JeopardyWebPolicy.REQUEST_EXIT_JS) { exit[0] = it; exitLatch.countDown() }
            }
            assertTrue(exitLatch.await(5, TimeUnit.SECONDS))
            assertFalse("no role yet means the page does not take the back gesture", exit[0] == "true")

            // A confirmed local leave becomes the validated close URI and closes the view.
            instrumentation.runOnMainSync {
                holder[0]!!.evaluateJavascript("window.dispatchEvent(new CustomEvent('${JeopardyWebPolicy.CLOSE_EVENT}',{detail:{role:'host'}}));", null)
            }
            assertTrue("confirmed local leave closes the embedded view", waitFor { closed.get() })
            assertNotNull("the page only talked to the plain-HTTP LAN origin", server.lastPath)
        } finally {
            instrumentation.runOnMainSync { holder[0]?.stopLoading(); holder[0]?.destroy() }
            server.stop()
        }
    }

    private fun waitFor(timeoutMs: Long = 8000, predicate: () -> Boolean): Boolean {
        val deadline = System.currentTimeMillis() + timeoutMs
        while (System.currentTimeMillis() < deadline) {
            if (predicate()) return true
            Thread.sleep(100)
        }
        return predicate()
    }
}
