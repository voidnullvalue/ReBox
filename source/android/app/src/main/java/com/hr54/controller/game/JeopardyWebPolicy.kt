package com.hr54.controller.game

import okhttp3.HttpUrl
import okhttp3.HttpUrl.Companion.toHttpUrl
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull

/** What the embedded controller may do with a URL the page tries to open. */
enum class UrlDecision { ALLOW, CLOSE, DENY }

/**
 * Pure, testable URL/origin policy for the embedded Jeopardy phone controller.
 *
 * The reviewed module page (`assets/index.html`) is served from the receiver's
 * own LAN origin on port [GAME_PORT] over plain HTTP and uses relative
 * `fetch('/api/...')` + `ws://<host>/ws` calls. The WebView is pinned to that
 * single origin: nothing external, no HTTPS upgrade, no `file:`/`content:`/
 * `javascript:`/`intent:` navigation, and no external app hand-off.
 */
object JeopardyWebPolicy {
    /** The module ID that owns the LAN phone controller page. */
    const val MODULE_ID = "jeopardy"

    /** Bundled asset path produced by the `generateJeopardyAssets` Gradle task. */
    const val ASSET_PATH = "jeopardy/index.html"

    /** LAN HTTP + WebSocket port owned by the Jeopardy receiver module. */
    const val GAME_PORT = 8135

    const val CLOSE_SCHEME = "rebox-controller"
    const val CLOSE_HOST = "close"

    /** Tightly-validated custom URI the injected listener navigates to on a confirmed local leave. */
    const val CLOSE_URI = "$CLOSE_SCHEME://$CLOSE_HOST"

    /** DOM event the reviewed page dispatches after a locally confirmed "Leave controller". */
    const val CLOSE_EVENT = "rebox-controller-close"

    private val ROLES = setOf("", "host", "player", "public")

    /**
     * Derive the game origin from a normalized receiver address. The core port is
     * discarded, HTTPS is forced to HTTP (the LAN game endpoint is plain HTTP) and
     * port [GAME_PORT] is set. IPv6 hosts are re-bracketed.
     */
    fun gameUrl(receiver: String): String {
        val parsed = receiver.toHttpUrl()
        val host = if (parsed.host.contains(':')) "[${parsed.host}]" else parsed.host
        return "http://$host:$GAME_PORT/"
    }

    /**
     * True when [candidate] is exactly the configured game HTTP origin. URLs that
     * smuggle credentials (`http://user:pass@host/`) are never the game origin.
     */
    fun isGameOrigin(candidate: String, gameUrl: String): Boolean {
        val target = candidate.toHttpUrlOrNull() ?: return false
        val game = gameUrl.toHttpUrlOrNull() ?: return false
        if (target.username.isNotEmpty() || target.password.isNotEmpty()) return false
        return target.scheme == "http" && game.scheme == "http" &&
            target.host == game.host && target.port == game.port
    }

    /**
     * The injected local-leave listener is only installed on the configured game
     * origin, never on an arbitrary page or a load-error page.
     */
    fun shouldInjectCloseListener(url: String?, gameUrl: String): Boolean =
        url != null && isGameOrigin(url, gameUrl)

    /**
     * Returns the confirmed role for a tightly-validated [CLOSE_URI], or null if
     * the URL is not exactly one. Extra params, credentials, paths or a role
     * outside the known set are rejected.
     */
    fun closeRole(url: String): String? {
        val prefix = "$CLOSE_URI?"
        if (!url.startsWith(prefix)) return null
        val query = url.substring(prefix.length)
        if (query.isEmpty() || query.length > 32) return null
        val params = query.split('&')
        if (params.size != 1) return null
        val (key, value) = params[0].split('=', limit = 2).let { it.getOrNull(0) to it.getOrNull(1) } // null value => reject
        if (key != "role" || value == null || value !in ROLES) return null
        return value
    }

    /** Classify a navigation/subresource URL against the configured game origin. */
    fun decide(url: String?, gameUrl: String): UrlDecision {
        if (url.isNullOrBlank()) return UrlDecision.DENY
        if (closeRole(url) != null) return UrlDecision.CLOSE
        return if (isGameOrigin(url, gameUrl)) UrlDecision.ALLOW else UrlDecision.DENY
    }

    /** Redacted origin for logging: never includes a path, query or fragment (which may carry a lobby PIN). */
    fun redact(url: String?): String {
        val parsed: HttpUrl = url?.toHttpUrlOrNull() ?: return "<invalid url>"
        return "${parsed.scheme}://${parsed.host}:${parsed.port}"
    }

    /**
     * The listener injected after first paint. It turns the page's local
     * `rebox-controller-close` DOM event into a single validated [CLOSE_URI]
     * navigation. It carries no credential and adds no Java bridge.
     */
    val CLOSE_LISTENER_JS: String = """
        (function(){
          if (window.__reboxJeopardyCloseHooked) return;
          window.__reboxJeopardyCloseHooked = true;
          window.addEventListener('$CLOSE_EVENT', function(event){
            var role = '';
            try { role = (event && event.detail && event.detail.role) ? String(event.detail.role) : ''; } catch (e) {}
            role = role.replace(/[^a-z]/g, '').slice(0, 8);
            try { window.location.href = '$CLOSE_URI?role=' + role; } catch (e) {}
          });
        })();
    """.trimIndent()

    /** Evaluates the reviewed Unit1 back/close contract; true when the page took the gesture. */
    const val REQUEST_EXIT_JS: String =
        "(function(){try{return !!(window.ReboxJeopardy&&window.ReboxJeopardy.requestExit&&window.ReboxJeopardy.requestExit());}catch(e){return false;}})()"
}
