package com.hr54.controller.game

import android.webkit.WebView
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.LifecycleOwner

/**
 * Owns the lifecycle of the single embedded controller WebView. Each instance is
 * paired with exactly one observer and released exactly once, so a Retry or a
 * receiver change can never destroy the freshly created WebView or leak the
 * previous instance's observer.
 */
class JeopardyWebViewLifecycle(private val owner: LifecycleOwner) {
    private val observers = HashMap<WebView, LifecycleEventObserver>()

    /** Registers one resume/pause forwarder for [view]. */
    fun observe(view: WebView) {
        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_RESUME -> view.onResume()
                Lifecycle.Event.ON_PAUSE -> view.onPause()
                else -> Unit
            }
        }
        observers[view] = observer
        owner.lifecycle.addObserver(observer)
    }

    /**
     * Releases exactly [view]: removes its observer and destroys it once. An
     * already-released or foreign instance is never touched, so a stale release
     * cannot tear down a replacement WebView.
     */
    fun release(view: WebView) {
        val observer = observers.remove(view) ?: return
        owner.lifecycle.removeObserver(observer)
        view.stopLoading()
        view.destroy()
    }
}
