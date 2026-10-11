package com.hr54.controller.ui

import android.webkit.WebView
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.lifecycle.compose.LocalLifecycleOwner
import com.hr54.controller.game.*
import com.hr54.controller.ui.components.AppIcons

/**
 * Full-screen embedded controller for the Jeopardy phone page. It hides the app
 * bottom navigation while playing, so the in-page BUZZ button owns the whole
 * touch surface. Back and the toolbar Close both evaluate the reviewed JS
 * `window.ReboxJeopardy.requestExit()`; the embedded view only closes when the
 * page confirms a local "Leave controller" (or when there is no role to guard).
 *
 * The whole surface respects `safeDrawingPadding`, so the header clears the
 * status bar, the bottom BUZZ clears the navigation gesture and the wager field
 * is reachable above the IME on every phone in portrait.
 */
@Composable
fun JeopardyControllerScreen(
    receiver: String,
    html: String,
    onClose: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val lifecycleOwner = LocalLifecycleOwner.current
    val gameUrl = remember(receiver) { JeopardyWebPolicy.gameUrl(receiver) }
    val webLifecycle = remember(lifecycleOwner) { JeopardyWebViewLifecycle(lifecycleOwner) }
    var status by remember(gameUrl) { mutableStateOf(JeopardyWebStatus.LOADING) }
    var progress by remember(gameUrl) { mutableIntStateOf(0) }
    var generation by remember(gameUrl) { mutableIntStateOf(0) }
    var health by remember(gameUrl) { mutableStateOf<JeopardyHealth?>(null) }
    var web by remember(gameUrl) { mutableStateOf<WebView?>(null) }

    // Bounded LAN health probe: only the configured HTTP game origin on 8135, no
    // redirect follow, no external host and no management token. It runs once per
    // receiver/Retry (never per frame) and cancels when the host changes or the
    // screen leaves.
    LaunchedEffect(gameUrl, generation) {
        health = null
        health = JeopardyHealthProbe.probe(gameUrl)
    }

    BackHandler { web?.let { requestJeopardyExit(it) { onClose() } } ?: onClose() }

    Surface(modifier.fillMaxSize()) {
        Column(Modifier.fillMaxSize().safeDrawingPadding()) {
            Row(
                Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Column(Modifier.weight(1f)) {
                    Text("Jeopardy! on this phone", style = MaterialTheme.typography.titleMedium)
                    Text(gameUrl, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                TextButton(onClick = { web?.let { requestJeopardyExit(it) { onClose() } } ?: onClose() }) {
                    Icon(AppIcons.ArrowBack, null); Text("Close")
                }
            }
            val h = health
            val help = h is JeopardyHealth.Unhealthy || (h is JeopardyHealth.Healthy && status == JeopardyWebStatus.ERROR)
            when {
                h == null -> Box(Modifier.weight(1f).fillMaxWidth(), contentAlignment = Alignment.Center) { CircularProgressIndicator() }
                help -> Box(Modifier.weight(1f).fillMaxWidth()) {
                    ConnectionHelp(
                        gameUrl = gameUrl,
                        detail = (h as? JeopardyHealth.Unhealthy)?.reason,
                        onRetry = { status = JeopardyWebStatus.LOADING; progress = 0; generation++ },
                        onClose = onClose,
                    )
                }
                else -> {
                    if (status == JeopardyWebStatus.LOADING) LinearProgressIndicator(
                        progress = { progress.coerceIn(0, 100) / 100f }, modifier = Modifier.fillMaxWidth()
                    )
                    Box(Modifier.weight(1f).fillMaxWidth()) {
                        // A new key disposes the old AndroidView, which releases exactly its
                        // own instance through onRelease; the replacement is never destroyed.
                        key(gameUrl, generation) {
                            AndroidView(
                                modifier = Modifier.fillMaxSize(),
                                factory = { ctx ->
                                    buildJeopardyWebView(
                                        context = ctx,
                                        gameUrl = gameUrl,
                                        html = html,
                                        onClose = onClose,
                                        onProgress = { if (status == JeopardyWebStatus.LOADING) progress = it },
                                        onReady = { status = JeopardyWebStatus.READY },
                                        onError = { status = JeopardyWebStatus.ERROR },
                                    ).also { view ->
                                        web = view
                                        webLifecycle.observe(view)
                                    }
                                },
                                onRelease = { view ->
                                    webLifecycle.release(view)
                                    if (web === view) web = null
                                },
                            )
                        }
                    }
                }
            }
        }
    }
}

@Composable
internal fun ConnectionHelp(
    gameUrl: String,
    detail: String?,
    onRetry: () -> Unit,
    onClose: () -> Unit,
) {
    Surface(color = MaterialTheme.colorScheme.surface.copy(alpha = 0.96f), modifier = Modifier.fillMaxSize()) {
        Column(
            Modifier.fillMaxSize().safeDrawingPadding().padding(24.dp).verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.Center,
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Text("Can't reach the game", style = MaterialTheme.typography.headlineSmall)
            Spacer(Modifier.height(12.dp))
            detail?.let {
                Text(it)
                Spacer(Modifier.height(8.dp))
            }
            Text(
                "Open $gameUrl from this phone on the receiver's LAN, and start Jeopardy! on the TV with the HR54 remote first. The TV owns the game; this phone is only a controller.",
            )
            Spacer(Modifier.height(4.dp))
            Text(gameUrl, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.primary)
            Spacer(Modifier.height(20.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                Button(onClick = onRetry) { Text("Retry") }
                OutlinedButton(onClick = onClose) { Text("Close") }
            }
        }
    }
}

/**
 * Shown when the bundled controller page cannot be read at all. It always offers
 * an accessible Close and Retry, so a broken bundle can never strand the phone on
 * an endless spinner, and Back maps to Close (never an implicit app exit).
 */
@Composable
internal fun ControllerAssetError(
    reason: String,
    onRetry: () -> Unit,
    onClose: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Surface(modifier.fillMaxSize()) {
        Column(
            Modifier.fillMaxSize().safeDrawingPadding().padding(24.dp).verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.Center,
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Text("Can't open the phone controller", style = MaterialTheme.typography.headlineSmall)
            Spacer(Modifier.height(12.dp))
            Text(reason)
            Spacer(Modifier.height(20.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                Button(onClick = onRetry) { Text("Retry") }
                OutlinedButton(onClick = onClose) { Text("Close") }
            }
        }
    }
}
