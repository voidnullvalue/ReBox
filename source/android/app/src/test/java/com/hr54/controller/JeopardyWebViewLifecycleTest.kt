package com.hr54.controller

import android.content.Context
import android.webkit.WebView
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.LifecycleRegistry
import com.hr54.controller.game.JeopardyWebViewLifecycle
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [36])
class JeopardyWebViewLifecycleTest {
    private class Owner : LifecycleOwner {
        val registry = LifecycleRegistry(this)
        override val lifecycle: Lifecycle get() = registry
    }

    private class CountingWebView(context: Context) : WebView(context) {
        var destroys = 0
        override fun destroy() { destroys++; super.destroy() }
    }

    @Test fun releaseDestroysExactlyTheReleasedInstanceOnce() {
        val lifecycle = JeopardyWebViewLifecycle(Owner())
        val app = RuntimeEnvironment.getApplication()
        val first = CountingWebView(app)
        val second = CountingWebView(app)
        lifecycle.observe(first)
        lifecycle.observe(second)

        lifecycle.release(first)
        assertEquals(1, first.destroys)
        assertEquals("a replacement instance is never destroyed", 0, second.destroys)

        // A stale/duplicate release is a no-op, so it can never tear down a replacement.
        lifecycle.release(first)
        assertEquals(1, first.destroys)
        assertEquals(0, second.destroys)

        lifecycle.release(second)
        assertEquals(1, second.destroys)
    }

    @Test fun releasingAnUntrackedInstanceNeverTouchesIt() {
        val lifecycle = JeopardyWebViewLifecycle(Owner())
        val app = RuntimeEnvironment.getApplication()
        val live = CountingWebView(app)
        val foreign = CountingWebView(app)
        lifecycle.observe(live)
        lifecycle.release(foreign)
        assertEquals(0, foreign.destroys)
        assertEquals(0, live.destroys)
    }

    @Test fun oneObserverPerInstanceIsAddedAndRemoved() {
        val owner = Owner()
        val lifecycle = JeopardyWebViewLifecycle(owner)
        val app = RuntimeEnvironment.getApplication()
        val first = CountingWebView(app)
        val second = CountingWebView(app)

        assertEquals(0, owner.registry.observerCount)
        lifecycle.observe(first)
        lifecycle.observe(second)
        assertEquals(2, owner.registry.observerCount)
        lifecycle.release(first)
        assertEquals(1, owner.registry.observerCount)
        lifecycle.release(second)
        assertEquals(0, owner.registry.observerCount)
    }
}
