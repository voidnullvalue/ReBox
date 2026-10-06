package com.hr54.controller.ui.components

import androidx.compose.animation.Crossfade
import androidx.compose.animation.core.tween
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.ui.draw.clip
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.grid.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.MotionDurationScale
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import coil.compose.SubcomposeAsyncImage
import coil.request.ImageRequest
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull
import androidx.compose.ui.platform.LocalContext
import com.hr54.controller.data.model.*

private val colors =
    darkColorScheme(
        background = Color(0xFF000000),
        surface = Color(0xFF000000),
        surfaceVariant = Color(0xFF121416),
        surfaceContainerLowest = Color(0xFF000000),
        surfaceContainerLow = Color(0xFF0A0B0D),
        surfaceContainer = Color(0xFF0E1012),
        surfaceContainerHigh = Color(0xFF121416),
        surfaceContainerHighest = Color(0xFF181B1E),
        primaryContainer = Color(0xFF103330),
        onPrimaryContainer = Color(0xFFB6F5EF),
        secondary = Color(0xFF9DC5C1),
        secondaryContainer = Color(0xFF122B29),
        outline = Color(0xFF52606C),
        outlineVariant = Color(0xFF262B2E),
        primary = Color(0xFF69D6CF),
        onPrimary = Color(0xFF003733),
        onSecondary = Color(0xFF092E2B),
        onSecondaryContainer = Color(0xFFB6F5EF),
        surfaceTint = Color.Transparent,
        inverseSurface = Color(0xFFE5E9EB),
        inverseOnSurface = Color(0xFF151719),
        inversePrimary = Color(0xFF006B65),
        onBackground = Color(0xFFF4F6F8),
        onSurface = Color(0xFFF4F6F8),
        onSurfaceVariant = Color(0xFF9CA6B1),
        error = Color(0xFFFF8F89),
    )

@Composable
fun Hr54Theme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = colors,
        shapes =
            Shapes(
                small = RoundedCornerShape(12.dp),
                medium = RoundedCornerShape(14.dp),
                large = RoundedCornerShape(16.dp),
            ),
        content = content,
    )
}

val LocalModules = staticCompositionLocalOf<List<ModuleDescriptor>> { emptyList() }
val LocalMediaPage = staticCompositionLocalOf<Pair<String?, List<MediaItem>>> { null to emptyList() }
@Composable fun sourceLabel(source: String?) = LocalModules.current.find { it.id == source }?.name ?: source.orEmpty()

@Composable
fun ConnectionIndicator(connected: Boolean, retry: () -> Unit) {
    TextButton(onClick = retry) {
        Text(
            if (connected) "● Connected" else "○ Disconnected · Retry",
            color =
                if (connected) MaterialTheme.colorScheme.primary
                else MaterialTheme.colorScheme.error,
            style = MaterialTheme.typography.labelMedium,
        )
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun Hr54TopBar(title: String, connected: Boolean, retry: () -> Unit, settings: () -> Unit) {
    TopAppBar(
        title = {
            Column {
                Text(title, style = MaterialTheme.typography.titleLarge)
                ConnectionIndicator(connected, retry)
            }
        },
        colors = TopAppBarDefaults.topAppBarColors(
            containerColor = MaterialTheme.colorScheme.background,
            scrolledContainerColor = MaterialTheme.colorScheme.background,
        ),
        actions = {
            IconButton(onClick = settings) { Icon(AppIcons.Settings, "Application settings") }
        },
    )
}

@Composable
fun SourceCard(name: String, onClick: () -> Unit) {
    val interaction = remember { MutableInteractionSource() }
    Card(onClick = onClick, interactionSource = interaction, modifier = Modifier.fillMaxWidth().then(pressMotion(interaction))) {
        Row(
            Modifier.padding(18.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Icon(AppIcons.Movie, null, tint = MaterialTheme.colorScheme.primary)
            Text(name, style = MaterialTheme.typography.titleMedium)
        }
    }
}

@Composable
fun Artwork(url: String?, modifier: Modifier = Modifier, description: String? = null) {
    val context = LocalContext.current
    val inspection = androidx.compose.ui.platform.LocalInspectionMode.current
    val motionEnabled = rememberCoroutineScope().coroutineContext[MotionDurationScale]?.scaleFactor != 0f
    val segments = url?.toHttpUrlOrNull()?.pathSegments.orEmpty()
    val id = segments.getOrNull(2).takeIf { segments.take(2) == listOf("api", "modules") }
    val version = LocalModules.current.find { it.id == id }?.version.orEmpty()
    val request = remember(url, version, context, inspection, motionEnabled) {
        ImageRequest.Builder(context).data(if (inspection) null else url).size(512)
            .memoryCacheKey("${version.length}:$version:$url").diskCacheKey("${version.length}:$version:$url")
            .crossfade(if (motionEnabled) 120 else 0).build()
    }
    SubcomposeAsyncImage(
        model = request,
        contentDescription = description,
        modifier = modifier.background(MaterialTheme.colorScheme.surfaceVariant),
        contentScale = ContentScale.Crop,
        loading = { ArtPlaceholder() },
        error = { ArtPlaceholder() },
    )
}

@Composable
private fun ArtPlaceholder() {
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Icon(AppIcons.Movie, null, tint = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
fun EmptyState(title: String, detail: String) {
    Column(
        Modifier.fillMaxWidth().padding(24.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Text(title, style = MaterialTheme.typography.titleMedium)
        Text(detail, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
fun InlineError(reason: String, retry: () -> Unit) {
    Column(Modifier.padding(16.dp)) {
        Text(reason, color = MaterialTheme.colorScheme.error)
        TextButton(onClick = retry) { Text("Retry") }
    }
}

@Composable
fun LoadingMediaGrid() {
    LazyVerticalGrid(
        GridCells.Adaptive(140.dp),
        contentPadding = PaddingValues(16.dp),
        horizontalArrangement = Arrangement.spacedBy(12.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        items(6) {
            LoadingSheen(
                Modifier.fillMaxWidth().height(190.dp).clip(RoundedCornerShape(12.dp))
            )
        }
    }
}

@Composable
fun TransportControls(
    playback: Playback,
    busy: Boolean,
    expanded: Boolean = false,
    onAction: (String, Int?) -> Unit,
) {
    val haptic = LocalHapticFeedback.current
    fun act(action: String, delta: Int? = null) {
        haptic.performHapticFeedback(HapticFeedbackType.LongPress)
        onAction(action, delta)
    }
    Row(
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        if (expanded && playback.transport.seek)
            IconButton(
                onClick = { act("seek", -30) },
                enabled = !busy,
                modifier = Modifier.semantics { contentDescription = "Back 30 seconds" },
            ) {
                Text("−30", style = MaterialTheme.typography.labelLarge)
            }
        if ((playback.paused && playback.transport.resume) || (!playback.paused && playback.transport.pause))
            IconButton(
                onClick = { act(if (playback.paused) "resume" else "pause") },
                enabled = !busy,
                modifier = Modifier.semantics {
                    contentDescription = if (playback.paused) "Resume on HR54" else "Pause on HR54"
                },
            ) {
                Crossfade(playback.paused, animationSpec = tween(100), label = "Transport state") { paused ->
                    Icon(if (paused) AppIcons.PlayArrow else AppIcons.Pause, null)
                }
            }
        if (expanded && playback.transport.seek)
            IconButton(
                onClick = { act("seek", 30) },
                enabled = !busy,
                modifier = Modifier.semantics { contentDescription = "Forward 30 seconds" },
            ) {
                Text("+30", style = MaterialTheme.typography.labelLarge)
            }
        if (playback.transport.stop)
            IconButton(onClick = { act("stop") }, enabled = !busy) {
                Icon(AppIcons.Stop, "Stop playback")
            }
    }
}

@Composable
private fun PlaybackArtwork(playback: Playback, base: String?, modifier: Modifier) {
    val id = playback.source
    val page = LocalMediaPage.current
    val key = if (page.first == id) page.second.find { it.id == playback.itemId }?.artwork?.takeIf { it.isNotEmpty() } else null
    Artwork(if (id != null && moduleId(id)) moduleAsset(base, id, key) else null, modifier)

}

@Composable
fun NowPlayingMiniBar(
    playback: Playback,
    base: String?,
    busy: Boolean,
    expand: () -> Unit,
    onAction: (String, Int?) -> Unit,
) {
    Surface(onClick = expand, color = MaterialTheme.colorScheme.surfaceVariant) {
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 6.dp)
                .then(destinationMotion(playback.itemId ?: playback.source.orEmpty())),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            PlaybackArtwork(playback, base, Modifier.size(44.dp))
            Column(Modifier.weight(1f).padding(horizontal = 12.dp)) {
                Text(
                    playback.title.ifBlank { playback.name },
                    maxLines = 1,
                    style = MaterialTheme.typography.titleSmall,
                )
                Text(
                    sourceLabel(playback.source),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            TransportControls(playback, busy, onAction = onAction)
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun NowPlayingSheet(
    playback: Playback,
    base: String?,
    duration: Double?,
    busy: Boolean,
    dismiss: () -> Unit,
    onAction: (String, Int?) -> Unit,
    seek: (Double) -> Unit,
) {
    ModalBottomSheet(onDismissRequest = dismiss) {
        NowPlayingContent(playback, base, duration, busy, onAction, seek)
        Spacer(Modifier.height(24.dp))
    }
}

@Composable
fun NowPlayingContent(
    playback: Playback,
    base: String?,
    duration: Double?,
    busy: Boolean,
    onAction: (String, Int?) -> Unit,
    seek: (Double) -> Unit,
) {
    Column(Modifier.padding(20.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Text(
            "NOW PLAYING",
            style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.primary,
        )
        if (playback.playing) {
            Row(horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                PlaybackArtwork(playback, base, Modifier.size(88.dp))
                Column {
                    Text(
                        playback.title.ifBlank { playback.name },
                        style = MaterialTheme.typography.titleLarge,
                    )
                    Text(
                        sourceLabel(playback.source),
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Text(
                        if (playback.live) "LIVE"
                        else
                            (if (playback.paused) "Paused · " else "") + timeLabel(playback.elapsed)
                    )
                }
            }
            TransportControls(playback, busy, true, onAction)
            if (playback.transport.seek && duration != null && duration > 0) {
                var drag by remember(playback.itemId) { mutableStateOf<Float?>(null) }
                Slider(
                    value = drag ?: playback.elapsed.toFloat().coerceIn(0f, duration.toFloat()),
                    onValueChange = { drag = it },
                    onValueChangeFinished = {
                        drag?.let { seek(it.toDouble()) }
                        drag = null
                    },
                    valueRange = 0f..duration.toFloat(),
                    enabled = !busy,
                )
                Text(
                    "${timeLabel(playback.elapsed)} / ${timeLabel(duration)} · approximate",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        } else {
            Text("HR54", style = MaterialTheme.typography.titleLarge)
            Text("Ready", color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}
