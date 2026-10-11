package com.hr54.controller

import android.net.Uri
import android.webkit.WebResourceRequest
import android.webkit.WebView
import com.hr54.controller.game.*
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import java.io.File

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [36])
class JeopardyWebViewTest {
    private val receiver = "http://192.168.88.103:8130"
    private val game = JeopardyWebPolicy.gameUrl(receiver)

    private class Request(private val value: String, private val mainFrame: Boolean = false) : WebResourceRequest {
        override fun getUrl(): Uri = Uri.parse(value)
        override fun isForMainFrame(): Boolean = mainFrame
        override fun isRedirect(): Boolean = false
        override fun hasGesture(): Boolean = false
        override fun getMethod(): String = "GET"
        override fun getRequestHeaders(): MutableMap<String, String> = mutableMapOf()
    }

    @Test fun settingsEnableOnlyJsAndDomStorageAndDisableFileContentAccess() {
        val web = WebView(RuntimeEnvironment.getApplication())
        configureJeopardySettings(web.settings)
        assertTrue(web.settings.javaScriptEnabled)
        assertTrue(web.settings.domStorageEnabled)
        assertFalse(web.settings.allowFileAccess)
        assertFalse(web.settings.allowContentAccess)
        assertFalse(web.settings.allowFileAccessFromFileURLs)
        assertFalse(web.settings.allowUniversalAccessFromFileURLs)
        assertFalse(web.settings.javaScriptCanOpenWindowsAutomatically)
        web.destroy()
    }

    @Test fun clientAllowsOnlyTheGameOriginAndInterceptsTheCloseUri() {
        var closed = 0; var errors = 0
        val client = JeopardyWebViewClient(game, { closed++ }, {}, { errors++ })
        val web = WebView(RuntimeEnvironment.getApplication())

        // Same origin proceeds (returns false), external/HTTPS/schemes are consumed (true).
        assertFalse(client.shouldOverrideUrlLoading(web, Request("http://192.168.88.103:8135/api/join", mainFrame = true)))
        assertTrue(client.shouldOverrideUrlLoading(web, Request("https://192.168.88.103:8135/", mainFrame = true)))
        assertTrue(client.shouldOverrideUrlLoading(web, Request("http://evil.test:8135/", mainFrame = true)))
        assertTrue(client.shouldOverrideUrlLoading(web, Request("intent://x/#Intent;end", mainFrame = true)))
        assertTrue(client.shouldOverrideUrlLoading(web, Request("javascript:alert(1)", mainFrame = true)))
        assertEquals(0, closed)
        // Each denied main-frame navigation (e.g. an HTTPS/external redirect) surfaces help.
        assertEquals(4, errors)

        // A confirmed local leave on the main frame closes the embedded view and never hands off.
        assertTrue(client.shouldOverrideUrlLoading(web, Request("${JeopardyWebPolicy.CLOSE_URI}?role=host", mainFrame = true)))
        assertEquals(1, closed)
        // A forged close URI is blocked without closing.
        assertTrue(client.shouldOverrideUrlLoading(web, Request("${JeopardyWebPolicy.CLOSE_URI}?role=admin", mainFrame = true)))
        assertEquals(1, closed)
        web.destroy()
    }

    @Test fun closeUriOnlyClosesTheMainFrameAndNeverAnIframe() {
        var closed = 0
        val client = JeopardyWebViewClient(game, { closed++ }, {}, {})
        val web = WebView(RuntimeEnvironment.getApplication())
        assertTrue(client.shouldOverrideUrlLoading(web, Request("${JeopardyWebPolicy.CLOSE_URI}?role=host", mainFrame = false)))
        assertEquals("an iframe close attempt never closes native", 0, closed)
        web.destroy()
    }

    @Test fun credentialBearingNavigationIsDeniedAndSurfacesHelp() {
        var errors = 0
        val client = JeopardyWebViewClient(game, {}, {}, { errors++ })
        val web = WebView(RuntimeEnvironment.getApplication())
        assertTrue(client.shouldOverrideUrlLoading(web, Request("http://user:pass@192.168.88.103:8135/", mainFrame = true)))
        assertEquals(1, errors)
        web.destroy()
    }

    @Test fun subresourceCloseUriIsBlockedWithoutClosing() {
        var closed = 0
        val client = JeopardyWebViewClient(game, { closed++ }, {}, {})
        val web = WebView(RuntimeEnvironment.getApplication())
        val blocked = client.shouldInterceptRequest(web, Request("${JeopardyWebPolicy.CLOSE_URI}?role=host"))
        assertNotNull(blocked)
        assertEquals(403, blocked!!.statusCode)
        assertEquals(0, closed)
        web.destroy()
    }

    @Test fun subresourceRequestsArePinnedToTheGameOrigin() {
        val client = JeopardyWebViewClient(game, {}, {}, {})
        val web = WebView(RuntimeEnvironment.getApplication())
        assertNull(client.shouldInterceptRequest(web, Request("http://192.168.88.103:8135/api/join")))
        val blocked = client.shouldInterceptRequest(web, Request("https://cdn.example.test/lib.js"))
        assertNotNull(blocked)
        assertEquals(403, blocked!!.statusCode)
        assertNull(client.shouldInterceptRequest(web, Request("http://192.168.88.103:8135/anything", mainFrame = true)))
        // A credential-bearing subresource is blocked, never proxied.
        assertNotNull(client.shouldInterceptRequest(web, Request("http://user:pass@192.168.88.103:8135/api/join")))
        web.destroy()
    }

    @Test fun bundledAssetMatchesTheModuleSourceByteForByte() {
        val bundled = RuntimeEnvironment.getApplication().assets.open(JeopardyWebPolicy.ASSET_PATH).use { it.readBytes() }
        val source = File(System.getProperty("hr54.repoRoot"), "source/hr54-re/modules/jeopardy/assets/index.html").readBytes()
        assertArrayEquals(source, bundled)
        assertTrue(String(bundled).contains("window.ReboxJeopardy"))
        assertTrue(String(bundled).contains(JeopardyWebPolicy.CLOSE_EVENT))
    }

    @Test fun selfContainedPageHasNoExternalCdnOrHttpsReferences() {
        val html = String(RuntimeEnvironment.getApplication().assets.open(JeopardyWebPolicy.ASSET_PATH).use { it.readBytes() })
        assertFalse(html.contains("https://"))
        assertFalse(html.contains("http://cdn"))
        assertFalse(html.contains("integrity="))
        assertTrue(html.contains("/api/join"))
        assertTrue(html.contains("new WebSocket"))
    }
}
