package com.hr54.controller.game

import android.annotation.SuppressLint
import android.content.Context
import android.util.Log
import android.webkit.WebChromeClient
import android.webkit.WebResourceError
import android.webkit.WebResourceRequest
import android.webkit.WebResourceResponse
import android.webkit.WebSettings
import android.webkit.WebView
import android.webkit.WebViewClient
import java.io.ByteArrayInputStream

private const val TAG = "JeopardyWebView"

enum class JeopardyWebStatus { LOADING, READY, ERROR }

/**
 * Applies the minimum WebView surface the reviewed page needs and nothing else:
 * JavaScript + DOM storage (identity is receiver-origin separated by the base URL),
 * HTTP audio behind a normal user gesture. File/content access and universal file
 * origins are off, and no Java bridge is ever installed.
 */
@SuppressLint("SetJavaScriptEnabled")
@Suppress("DEPRECATION")
fun configureJeopardySettings(settings: WebSettings) {
    settings.javaScriptEnabled = true
    settings.domStorageEnabled = true
    settings.allowFileAccess = false
    settings.allowContentAccess = false
    settings.allowFileAccessFromFileURLs = false
    settings.allowUniversalAccessFromFileURLs = false
    settings.javaScriptCanOpenWindowsAutomatically = false
    settings.setSupportMultipleWindows(false)
    // Receiver-side WebAudio is only unlocked by a genuine touch on the page.
    settings.mediaPlaybackRequiresUserGesture = true
    // The game endpoint is plain HTTP on the LAN; never silently upgrade.
    settings.mixedContentMode = WebSettings.MIXED_CONTENT_NEVER_ALLOW
    settings.cacheMode = WebSettings.LOAD_DEFAULT
    settings.setSupportZoom(false)
    settings.builtInZoomControls = false
    settings.displayZoomControls = false
    settings.loadsImagesAutomatically = true
}

/**
 * Restricts the WebView to the configured game origin. HTTPS upgrades, external
 * hosts, `intent:`, `javascript:` and `file:`/`content:` URLs are denied; the
 * single exception is the tightly-validated local-close URI.
 */
class JeopardyWebViewClient(
    private val gameUrl: String,
    private val onClose: () -> Unit,
    private val onReady: () -> Unit,
    private val onError: () -> Unit,
) : WebViewClient() {

    override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest): Boolean =
        handle(request.url?.toString(), request.isForMainFrame)

    @Suppress("OVERRIDE_DEPRECATION")
    override fun shouldOverrideUrlLoading(view: WebView, url: String): Boolean = handle(url, mainFrame = true)

    private fun handle(url: String?, mainFrame: Boolean): Boolean =
        when (JeopardyWebPolicy.decide(url, gameUrl)) {
            UrlDecision.ALLOW -> false
            // Only a main-frame close navigation closes the view; an iframe/subframe
            // attempt is consumed without touching native state.
            UrlDecision.CLOSE -> {
                if (mainFrame) onClose() else Log.w(TAG, "Ignored subframe close attempt")
                true
            }
            UrlDecision.DENY -> {
                Log.w(TAG, "Denied ${if (mainFrame) "main-frame" else "subframe"} navigation to ${JeopardyWebPolicy.redact(url)}")
                // A denied main-frame navigation (e.g. an HTTPS/external redirect) shows
                // the native help instead of silently blanking the controller.
                if (mainFrame) onError()
                true
            }
        }

    override fun shouldInterceptRequest(view: WebView, request: WebResourceRequest): WebResourceResponse? {
        // The base document is injected with loadDataWithBaseURL; any real main-frame
        // navigation is already vetted by shouldOverrideUrlLoading.
        if (request.isForMainFrame) return null
        val url = request.url?.toString()
        if (JeopardyWebPolicy.decide(url, gameUrl) == UrlDecision.ALLOW) return null
        Log.w(TAG, "Blocked subresource from ${JeopardyWebPolicy.redact(url)}")
        return blocked()
    }

    override fun onPageFinished(view: WebView, url: String) {
        // Only the configured game origin is a live reviewed page; never inject the
        // listener (or report ready) for an arbitrary or load-error page.
        if (JeopardyWebPolicy.shouldInjectCloseListener(url, gameUrl)) {
            view.evaluateJavascript(JeopardyWebPolicy.CLOSE_LISTENER_JS, null)
            onReady()
        } else {
            Log.w(TAG, "Skipped controller listener for non-game page ${JeopardyWebPolicy.redact(url)}")
        }
    }

    override fun onReceivedError(view: WebView, request: WebResourceRequest, error: WebResourceError) {
        if (request.isForMainFrame) {
            Log.w(TAG, "Main-frame load failed at ${JeopardyWebPolicy.redact(request.url?.toString())}")
            onError()
        }
    }

    // onReceivedSslError is intentionally NOT overridden: no SSL validation bypass.

    private fun blocked(): WebResourceResponse =
        WebResourceResponse("text/plain", "utf-8", 403, "Blocked", mapOf("Cache-Control" to "no-store"), ByteArrayInputStream(ByteArray(0)))
}

/**
 * Builds the embedded controller WebView. The reviewed page is injected with
 * `loadDataWithBaseURL` so relative `/api/...` fetch and `ws://<host>/ws` resolve
 * to the explicit receiver origin, while DOM storage stays keyed to that origin.
 */
fun buildJeopardyWebView(
    context: Context,
    gameUrl: String,
    html: String,
    onClose: () -> Unit,
    onProgress: (Int) -> Unit,
    onReady: () -> Unit,
    onError: () -> Unit,
): WebView {
    val web = WebView(context)
    configureJeopardySettings(web.settings)
    web.isVerticalScrollBarEnabled = false
    web.isHorizontalScrollBarEnabled = false
    web.webViewClient = JeopardyWebViewClient(gameUrl, onClose, onReady, onError)
    web.webChromeClient = object : WebChromeClient() {
        override fun onProgressChanged(view: WebView, newProgress: Int) { onProgress(newProgress) }
    }
    web.loadDataWithBaseURL(gameUrl, html, "text/html", "utf-8", null)
    return web
}

/**
 * Runs the reviewed JS exit contract. [onLeave] fires only when the page has no
 * role to guard (fresh/left page); otherwise the page modal owns the gesture and
 * a confirmed local leave arrives later as the custom close URI.
 */
fun requestJeopardyExit(web: WebView, onLeave: () -> Unit) {
    try {
        web.evaluateJavascript(JeopardyWebPolicy.REQUEST_EXIT_JS) { value ->
            if (value != "true") onLeave()
        }
    } catch (e: Exception) {
        Log.w(TAG, "Exit evaluation failed")
        onLeave()
    }
}
