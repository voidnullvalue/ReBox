package com.hr54.controller.ui

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.grid.*
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import com.hr54.controller.*
import com.hr54.controller.data.model.*
import com.hr54.controller.ui.components.*

@Composable
fun HomeScreen(
    s: ControllerState,
    navigate: (String) -> Unit,
    action: (String, Int?) -> Unit,
    seek: (Double) -> Unit,
    modifier: Modifier = Modifier,
) {
    LazyVerticalGrid(
        GridCells.Adaptive(160.dp),
        modifier = modifier,
        contentPadding = PaddingValues(16.dp),
        horizontalArrangement = Arrangement.spacedBy(12.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        item(span = { GridItemSpan(maxLineSpan) }) {
            Card {
                NowPlayingContent(
                    s.playback,
                    s.receiver,
                    s.playback.duration ?: s.knownDurations[s.playback.itemId],
                    s.busy,
                    action,
                    seek,
                )
            }
        }
        item(span = { GridItemSpan(maxLineSpan) }) {
            Text(
                "YOUR SOURCES",
                style = MaterialTheme.typography.labelLarge,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = 12.dp),
            )
        }
        items(s.capabilities.destinations().drop(1)) { SourceCard(it) { navigate(it) } }
    }
}

@Composable
fun JellyfinScreen(
    s: ControllerState,
    query: TextFieldValue,
    onQuery: (TextFieldValue) -> Unit,
    search: () -> Unit,
    startAuth: () -> Unit,
    select: (Item) -> Unit,
    more: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Column(modifier) {
        if (!s.auth.authenticated) {
            EmptyState(
                "Connect your Jellyfin library",
                "Approve Quick Connect in Jellyfin. Your other sources remain available.",
            )
            s.auth.code?.let {
                Text(
                    it,
                    style = MaterialTheme.typography.displayMedium,
                    color = MaterialTheme.colorScheme.primary,
                    modifier = Modifier.padding(24.dp),
                )
                Text(
                    if (s.auth.expired) "Code expired. Start again." else "Waiting for approval…",
                    modifier = Modifier.padding(horizontal = 24.dp),
                )
            }
            Button(onClick = startAuth, enabled = !s.loading, modifier = Modifier.padding(16.dp)) {
                Text(
                    if (s.auth.pending && !s.auth.expired) "New Quick Connect code"
                    else "Start Quick Connect"
                )
            }
            return@Column
        }
        WhisperTextField(
            query,
            onQuery,
            "Search Jellyfin",
            onSearch = search,
            modifier = Modifier.fillMaxWidth().padding(16.dp),
        )
        Text(
            s.folders.lastOrNull()?.name ?: "Libraries",
            style = MaterialTheme.typography.titleSmall,
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
        )
        if (s.loading && s.items.isEmpty()) LoadingMediaGrid()
        else
            LazyVerticalGrid(
                GridCells.Adaptive(125.dp),
                contentPadding = PaddingValues(16.dp),
                horizontalArrangement = Arrangement.spacedBy(12.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                // The receiver can return repeated IDs (for example Series seasons).
                // Keep raw rows in state for server pagination; render each media item once.
                items(s.items.distinctBy(Item::id), key = { it.id }) {
                    MediaPosterCard(it, s.receiver) { select(it) }
                }
                if (s.items.size < s.itemTotal)
                    item(span = { GridItemSpan(maxLineSpan) }) {
                        TextButton(onClick = more, enabled = !s.loading) { Text("Load more") }
                    }
                if (s.items.isEmpty() && !s.loading)
                    item(span = { GridItemSpan(maxLineSpan) }) {
                        EmptyState("No items", "Try another search or library.")
                    }
            }
    }
}

@Composable
fun IptvScreen(
    s: ControllerState,
    query: TextFieldValue,
    onQuery: (TextFieldValue) -> Unit,
    search: () -> Unit,
    group: (String) -> Unit,
    play: (Channel) -> Unit,
    more: () -> Unit,
    wide: Boolean,
    modifier: Modifier = Modifier,
) {
    Column(modifier) {
        WhisperTextField(
            query,
            onQuery,
            "Search channels",
            onSearch = search,
            modifier = Modifier.fillMaxWidth().padding(16.dp),
        )
        Row(Modifier.weight(1f)) {
            if (wide)
                LazyColumn(
                    Modifier.width(180.dp).fillMaxHeight(),
                    contentPadding = PaddingValues(12.dp),
                ) {
                    item {
                        FilterChip(
                            selected = s.group.isEmpty(),
                            onClick = { group("") },
                            label = { Text("All channels") },
                        )
                    }
                    items(s.groups, key = { it.name }) {
                        FilterChip(
                            selected = s.group == it.name,
                            onClick = { group(it.name) },
                            label = { Text("${it.name} (${it.count})") },
                        )
                    }
                }
            Column(Modifier.weight(1f)) {
                if (!wide)
                    LazyRow(
                        contentPadding = PaddingValues(horizontal = 16.dp),
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        item {
                            FilterChip(
                                selected = s.group.isEmpty(),
                                onClick = { group("") },
                                label = { Text("All") },
                            )
                        }
                        items(s.groups, key = { it.name }) {
                            FilterChip(
                                selected = s.group == it.name,
                                onClick = { group(it.name) },
                                label = { Text(it.name) },
                            )
                        }
                    }
                if (s.loading && s.channels.isEmpty()) LoadingMediaGrid()
                else
                    LazyVerticalGrid(
                        GridCells.Adaptive(if (wide) 280.dp else 320.dp),
                        contentPadding = PaddingValues(16.dp),
                        horizontalArrangement = Arrangement.spacedBy(12.dp),
                        verticalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        items(s.channels, key = { it.id }) { ChannelCard(it) { play(it) } }
                        if (s.channelMore)
                            item(span = { GridItemSpan(maxLineSpan) }) {
                                TextButton(onClick = more, enabled = !s.loading) {
                                    Text("Load more channels")
                                }
                            }
                        if (s.channels.isEmpty() && !s.loading)
                            item(span = { GridItemSpan(maxLineSpan) }) {
                                EmptyState("No channels", "Try a different group or query.")
                            }
                    }
            }
        }
    }
}

@Composable
fun YoutubeScreen(
    s: ControllerState,
    query: TextFieldValue,
    onQuery: (TextFieldValue) -> Unit,
    search: () -> Unit,
    play: (Video) -> Unit,
    more: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Column(modifier) {
        WhisperTextField(
            query,
            onQuery,
            "Search YouTube",
            onSearch = search,
            modifier = Modifier.fillMaxWidth().padding(16.dp),
        )
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 16.dp),
            horizontalArrangement = Arrangement.End,
        ) {
            Button(onClick = search, enabled = !s.loading && query.text.isNotBlank()) {
                Text("Search")
            }
        }
        if (s.loading) {
            LinearProgressIndicator(Modifier.fillMaxWidth().padding(16.dp))
            Text(
                "Searching YouTube… This can take up to a minute.",
                modifier = Modifier.padding(horizontal = 16.dp),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        LazyVerticalGrid(
            GridCells.Adaptive(300.dp),
            contentPadding = PaddingValues(16.dp),
            horizontalArrangement = Arrangement.spacedBy(16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            items(s.videos, key = { it.id }) { video ->
                val interaction = remember { MutableInteractionSource() }
                Card(onClick = { play(video) }, enabled = !s.busy && video.thumbnail != null, interactionSource = interaction, modifier = pressMotion(interaction)) {
                    Artwork(video.thumbnail, Modifier.fillMaxWidth().aspectRatio(16f / 9))
                    Column(Modifier.padding(12.dp)) {
                        Text(
                            video.title,
                            style = MaterialTheme.typography.titleMedium,
                            maxLines = 3,
                        )
                        Text(
                            listOfNotNull(video.channel, video.duration?.let(::timeLabel))
                                .joinToString(" · "),
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            style = MaterialTheme.typography.bodyMedium,
                        )
                    }
                }
            }
            if (s.videoMore)
                item(span = { GridItemSpan(maxLineSpan) }) {
                    TextButton(onClick = more, enabled = !s.loading) { Text("More results") }
                }
            if (s.videos.isEmpty() && !s.loading)
                item(span = { GridItemSpan(maxLineSpan) }) {
                    EmptyState(
                        "Find something for the television",
                        "Search videos, music and more. Playback stays on HR54.",
                    )
                }
        }
    }
}

@Composable
fun CamerasScreen(s: ControllerState, play: (Camera) -> Unit, modifier: Modifier = Modifier) {
    if (s.loading && s.cameras.isEmpty()) LoadingMediaGrid()
    else
        LazyVerticalGrid(
            GridCells.Adaptive(170.dp),
            modifier = modifier,
            contentPadding = PaddingValues(16.dp),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            items(s.cameras, key = { it.id }) { CameraCard(it) { play(it) } }
            if (s.cameras.isEmpty() && !s.loading)
                item(span = { GridItemSpan(maxLineSpan) }) {
                    EmptyState(
                        "No cameras available",
                        "Camera availability comes from the receiver.",
                    )
                }
        }
}

@Composable
fun ReceiverSetup(
    s: ControllerState,
    value: TextFieldValue,
    onValue: (TextFieldValue) -> Unit,
    connect: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Box(modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Column(
            Modifier.widthIn(max = 480.dp).padding(24.dp),
            verticalArrangement = Arrangement.spacedBy(20.dp),
        ) {
            Text(
                "HR54",
                style = MaterialTheme.typography.displaySmall,
                color = MaterialTheme.colorScheme.primary,
            )
            Text("Your television.\nYour media.", style = MaterialTheme.typography.headlineMedium)
            Text(
                "Connect to your receiver to browse and play on the big screen.",
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            WhisperTextField(
                value,
                onValue,
                "Receiver address",
                maxBytes = 256,
                modifier = Modifier.fillMaxWidth(),
                onSearch = connect,
            )
            Text(
                "192.168.88.103  ·  Default port 8130",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            s.error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
            Button(
                onClick = connect,
                enabled = !s.loading && value.text.isNotBlank(),
                modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp),
            ) {
                Text(if (s.loading) "Connecting…" else "Connect to receiver")
            }
            if (s.loading) LinearProgressIndicator(Modifier.fillMaxWidth())
        }
    }
}

@Composable
fun SettingsScreen(
    s: ControllerState,
    value: TextFieldValue,
    onValue: (TextFieldValue) -> Unit,
    connect: () -> Unit,
    test: () -> Unit,
    modelStatus: String,
    logout: () -> Unit,
) {
    LazyColumn(
        contentPadding = PaddingValues(20.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        item {
            Text("Receiver", style = MaterialTheme.typography.titleLarge)
            Text(s.receiver.orEmpty(), color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        item {
            WhisperTextField(
                value,
                onValue,
                "Receiver address",
                maxBytes = 256,
                onSearch = connect,
                modifier = Modifier.fillMaxWidth(),
            )
        }
        item {
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                OutlinedButton(onClick = test) { Text("Test connection") }
                Button(onClick = connect, enabled = !s.loading) { Text("Change receiver") }
            }
        }
        item {
            Text("Local speech", style = MaterialTheme.typography.titleLarge)
            Text(modelStatus, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Text(
                "Bundled base.en · quantized Q5_1 · English\nAudio and transcripts stay on this device.",
                modifier = Modifier.padding(top = 8.dp),
            )
        }
        if (s.capabilities.jellyfin && s.auth.authenticated)
            item { OutlinedButton(onClick = logout) { Text("Sign out of Jellyfin") } }
        item {
            Text(
                "HR54 ${BuildConfig.VERSION_NAME} (${BuildConfig.VERSION_CODE}) · ${BuildConfig.BUILD_TYPE}",
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Text("Native television media controller", style = MaterialTheme.typography.bodySmall)
        }
    }
}

val previewState =
    ControllerState(
        receiver = "http://receiver:8130",
        ready = true,
        connected = true,
        capabilities = Capabilities(true, true, true, true, true),
        playback =
            Playback(
                true,
                "jellyfin",
                title = "Alien",
                itemId = "alien",
                elapsed = 1394.0,
                transport = Transport(true, true, true, true),
            ),
        auth = Auth(true),
        items =
            listOf(
                Item("alien", "Alien", "Movie", 1979, 70200000000, playable = true),
                Item("arrival", "Arrival", "Movie", 2016, playable = true),
            ),
        channels =
            listOf(
                Channel("ch-1", "BBC World News", group = "News"),
                Channel("ch-2", "Discovery", group = "Entertainment"),
            ),
        groups = listOf(Group("News", 32), Group("Sports", 64)),
        videos = listOf(Video("jNQXAC9IVRw", "Me at the zoo", "jawed", 19.0)),
        cameras =
            listOf(
                Camera("driveway", "Driveway", playable = true),
                Camera("porch", "Front porch", reason = "H.264 restream unavailable"),
            ),
    )

@Preview(showBackground = true, widthDp = 412, heightDp = 800)
@Composable
fun HomePreview() {
    Hr54Theme { Surface { HomeScreen(previewState, {}, { _, _ -> }, {}) } }
}

@Preview(showBackground = true, widthDp = 412, heightDp = 800)
@Composable
fun JellyfinPreview() {
    Hr54Theme { Surface { JellyfinScreen(previewState, TextFieldValue(""), {}, {}, {}, {}, {}) } }
}

@Preview(showBackground = true, widthDp = 412, heightDp = 800)
@Composable
fun IptvPreview() {
    Hr54Theme {
        Surface { IptvScreen(previewState, TextFieldValue(""), {}, {}, {}, {}, {}, false) }
    }
}

@Preview(showBackground = true, widthDp = 412, heightDp = 800)
@Composable
fun YoutubePreview() {
    Hr54Theme {
        Surface { YoutubeScreen(previewState, TextFieldValue("wildlife"), {}, {}, {}, {}) }
    }
}

@Preview(showBackground = true, widthDp = 412, heightDp = 800)
@Composable
fun CamerasPreview() {
    Hr54Theme { Surface { CamerasScreen(previewState, {}) } }
}

@Preview(showBackground = true, widthDp = 412)
@Composable
fun NowPlayingPreview() {
    Hr54Theme {
        Surface {
            NowPlayingContent(
                previewState.playback,
                previewState.receiver,
                7020.0,
                false,
                { _, _ -> },
                {},
            )
        }
    }
}
