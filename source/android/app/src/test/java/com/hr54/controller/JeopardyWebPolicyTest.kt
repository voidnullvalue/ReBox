package com.hr54.controller

import com.hr54.controller.game.JeopardyWebPolicy
import com.hr54.controller.game.UrlDecision
import org.junit.Assert.*
import org.junit.Test

class JeopardyWebPolicyTest {
    private val receiver = "http://192.168.88.103:8130"
    private val game = JeopardyWebPolicy.gameUrl(receiver)

    @Test fun gameUrlDiscardsCorePortForcesHttpAndKeepsHost() {
        assertEquals("http://192.168.88.103:8135/", game)
        // A normalized HTTPS receiver still yields the plain-HTTP LAN game origin.
        assertEquals("http://receiver:8135/", JeopardyWebPolicy.gameUrl("https://receiver:9443"))
        assertEquals("http://receiver:8135/", JeopardyWebPolicy.gameUrl("http://receiver:80"))
    }

    @Test fun gameUrlReBracketsIpv6Hosts() {
        assertEquals("http://[fd00::1]:8135/", JeopardyWebPolicy.gameUrl("http://[fd00::1]:8130"))
        assertEquals("http://[fe80::1]:8135/", JeopardyWebPolicy.gameUrl("https://[fe80::1]:9443"))
    }

    @Test fun gameUrlIsIdempotentPerReceiverButSeparatesIdentity() {
        // Same receiver -> same origin, so DOM storage (player/host identity) resumes.
        assertEquals(game, JeopardyWebPolicy.gameUrl("http://192.168.88.103:8130"))
        // A different receiver gets a different origin, so identities never leak.
        assertNotEquals(game, JeopardyWebPolicy.gameUrl("http://192.168.88.104:8130"))
    }

    @Test fun onlyTheConfiguredGameOriginIsAllowed() {
        assertEquals(UrlDecision.ALLOW, JeopardyWebPolicy.decide("http://192.168.88.103:8135/", game))
        assertEquals(UrlDecision.ALLOW, JeopardyWebPolicy.decide("http://192.168.88.103:8135/api/join", game))
        assertEquals(UrlDecision.ALLOW, JeopardyWebPolicy.decide("http://192.168.88.103:8135/ws", game))
    }

    @Test fun httpsUpgradesExternalHostsAndSubdomainTricksAreDenied() {
        assertEquals(UrlDecision.DENY, JeopardyWebPolicy.decide("https://192.168.88.103:8135/", game))
        assertEquals(UrlDecision.DENY, JeopardyWebPolicy.decide("http://192.168.88.103:8130/", game))
        assertEquals(UrlDecision.DENY, JeopardyWebPolicy.decide("http://evil.test:8135/", game))
        assertEquals(UrlDecision.DENY, JeopardyWebPolicy.decide("http://192.168.88.103:8135.evil.test/", game))
        assertEquals(UrlDecision.DENY, JeopardyWebPolicy.decide("http://192.168.88.103/", game))
    }

    @Test fun dangerousSchemesAreDenied() {
        for (url in listOf(
            "javascript:alert(1)",
            "file:///etc/passwd",
            "content://com.example/secret",
            "intent://scan/#Intent;scheme=zxing;end",
            "data:text/html,<script>1</script>",
            "http://169.254.169.254/latest/meta-data/",
        )) assertEquals("denied $url", UrlDecision.DENY, JeopardyWebPolicy.decide(url, game))
        assertEquals(UrlDecision.DENY, JeopardyWebPolicy.decide(null, game))
        assertEquals(UrlDecision.DENY, JeopardyWebPolicy.decide("", game))
    }

    @Test fun onlyTheTightlyValidatedCloseUriIsIntercepted() {
        assertEquals(UrlDecision.CLOSE, JeopardyWebPolicy.decide("${JeopardyWebPolicy.CLOSE_URI}?role=host", game))
        assertEquals(UrlDecision.CLOSE, JeopardyWebPolicy.decide("${JeopardyWebPolicy.CLOSE_URI}?role=", game))
        assertEquals("player", JeopardyWebPolicy.closeRole("${JeopardyWebPolicy.CLOSE_URI}?role=player"))
        assertEquals("", JeopardyWebPolicy.closeRole("${JeopardyWebPolicy.CLOSE_URI}?role="))
        // Missing query, extra params, unknown roles or smuggled values are never a close.
        for (url in listOf(
            JeopardyWebPolicy.CLOSE_URI,
            "${JeopardyWebPolicy.CLOSE_URI}?role=host&extra=1",
            "${JeopardyWebPolicy.CLOSE_URI}?role=admin",
            "${JeopardyWebPolicy.CLOSE_URI}?role=host/../x",
            "${JeopardyWebPolicy.CLOSE_URI}?role=host%00",
            "${JeopardyWebPolicy.CLOSE_URI}-evil?role=host",
        )) {
            assertNull(url, JeopardyWebPolicy.closeRole(url))
            assertEquals("denied $url", UrlDecision.DENY, JeopardyWebPolicy.decide(url, game))
        }
    }

    @Test fun credentialBearingUrlsAreNeverTheGameOrigin() {
        assertFalse(JeopardyWebPolicy.isGameOrigin("http://user:pass@192.168.88.103:8135/", game))
        assertFalse(JeopardyWebPolicy.isGameOrigin("http://user@192.168.88.103:8135/api/join", game))
        assertEquals(UrlDecision.DENY, JeopardyWebPolicy.decide("http://user:pass@192.168.88.103:8135/", game))
        assertEquals(UrlDecision.DENY, JeopardyWebPolicy.decide("http://user@192.168.88.103:8135/api/join", game))
        // The clean origin is still allowed.
        assertTrue(JeopardyWebPolicy.isGameOrigin("http://192.168.88.103:8135/api/join", game))
    }

    @Test fun closeListenerIsOnlyInjectedOnTheConfiguredGameOrigin() {
        assertTrue(JeopardyWebPolicy.shouldInjectCloseListener("http://192.168.88.103:8135/", game))
        assertTrue(JeopardyWebPolicy.shouldInjectCloseListener("http://192.168.88.103:8135/room?pin=1", game))
        assertFalse(JeopardyWebPolicy.shouldInjectCloseListener("https://192.168.88.103:8135/", game))
        assertFalse(JeopardyWebPolicy.shouldInjectCloseListener("http://evil.test:8135/", game))
        assertFalse(JeopardyWebPolicy.shouldInjectCloseListener("http://192.168.88.103:8130/", game))
        assertFalse(JeopardyWebPolicy.shouldInjectCloseListener(null, game))
        assertFalse(JeopardyWebPolicy.shouldInjectCloseListener("data:text/html,<p>error</p>", game))
    }

    @Test fun logRedactionStripsPathQueryAndFragment() {
        assertEquals("http://192.168.88.103:8135", JeopardyWebPolicy.redact("http://192.168.88.103:8135/api/join?pin=1234#pin=1234"))
        assertEquals("<invalid url>", JeopardyWebPolicy.redact("javascript:alert(1)"))
    }

    @Test fun reviewedUnitOneContractsAreWiredVerbatim() {
        assertTrue(JeopardyWebPolicy.REQUEST_EXIT_JS.contains("window.ReboxJeopardy"))
        assertTrue(JeopardyWebPolicy.REQUEST_EXIT_JS.contains("requestExit"))
        assertTrue(JeopardyWebPolicy.CLOSE_LISTENER_JS.contains(JeopardyWebPolicy.CLOSE_EVENT))
        assertTrue(JeopardyWebPolicy.CLOSE_LISTENER_JS.contains(JeopardyWebPolicy.CLOSE_URI))
        // No Java bridge ever enters the page.
        assertFalse(JeopardyWebPolicy.CLOSE_LISTENER_JS.contains("addJavascriptInterface"))
    }
}
