package com.hr54.controller.ui

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.*
import androidx.compose.foundation.lazy.grid.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.*
import androidx.compose.ui.unit.dp
import androidx.compose.ui.tooling.preview.Preview
import com.hr54.controller.*
import com.hr54.controller.data.model.*
import com.hr54.controller.ui.components.*
import kotlinx.serialization.json.*

@Composable
fun HomeScreen(s: ControllerState, navigate: (String) -> Unit, action: (String, Int?) -> Unit, seek: (Double) -> Unit, modifier: Modifier = Modifier) {
    LazyColumn(modifier, contentPadding = PaddingValues(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        item { Text("Your television", style = MaterialTheme.typography.headlineMedium) }
        if (s.playback.playing) item { NowPlayingContent(s.playback, s.receiver, s.playback.duration, s.busy, action, seek) }
        if (s.system.nativeModule.isNotEmpty()) item { Text("Running: ${s.module(s.system.nativeModule)?.name ?: s.system.nativeModule}") }
        items(s.homeModules, key = { it.id }) { m ->
            Card(onClick = { navigate(m.id) }, modifier = Modifier.fillMaxWidth()) {
                Row(Modifier.padding(16.dp), horizontalArrangement = Arrangement.spacedBy(16.dp), verticalAlignment = Alignment.CenterVertically) {
                    Artwork(moduleAsset(s.receiver, m.id), Modifier.size(64.dp), m.name)
                    Column(Modifier.weight(1f)) {
                        Text(m.name, style = MaterialTheme.typography.titleMedium)
                        Text(if (m.healthy) m.description else m.error.ifBlank { "Module unavailable" }, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                }
            }
        }
        if (s.homeModules.isEmpty()) item { EmptyState("No enabled modules", "Open Settings → Modules to install or enable a module.") }
    }
}
@Composable
fun ModuleScreen(s: ControllerState, query: TextFieldValue, onQuery: (TextFieldValue) -> Unit, search: () -> Unit,
    select: (MediaItem) -> Unit, more: () -> Unit, previous: () -> Unit, settings: () -> Unit, native: (Boolean) -> Unit, modifier: Modifier = Modifier) {
    val m = s.module(s.activeModule) ?: return
    Column(modifier) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp), horizontalArrangement = Arrangement.SpaceBetween) {
            Text(s.cursor.title.ifBlank { m.name }, style = MaterialTheme.typography.titleMedium)
            if (m.capabilities.settings) TextButton(onClick = settings) { Text("Module settings") }
        }
        if (m.kind == "native-app") {
            EmptyState(m.name, m.description)
            if (m.presentation.releaseSurface || m.presentation.releaseInput) Text("Launch with the HR54 remote so the receiver shell can release its display and input. You can stop the app here.", Modifier.padding(horizontal = 16.dp))
            Row(Modifier.padding(16.dp), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                Button(onClick = { native(true) }, enabled = !s.busy && !s.system.mediaBusy && !m.presentation.releaseSurface && !m.presentation.releaseInput) { Text("Launch on HR54") }
                if (s.system.nativeModule == m.id) OutlinedButton(onClick = { native(false) }, enabled = !s.busy) { Text("Stop app") }
            }
        } else {
            if (m.capabilities.search) WhisperTextField(query, onQuery, "Search ${m.name}", onSearch = search, modifier = Modifier.fillMaxWidth().padding(16.dp))
            if (s.page.items.isEmpty() && !s.loading) EmptyState(if (m.capabilities.browse || s.cursor.query.isNotEmpty()) "No items" else "Search this module", "Browse folders or enter a search above.")
            LazyVerticalGrid(GridCells.Adaptive(170.dp), contentPadding = PaddingValues(16.dp), horizontalArrangement = Arrangement.spacedBy(12.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                items(s.page.items.distinctBy { it.id }, key = { it.id }) { item ->
                    Card(onClick = { select(item) }, enabled = !s.loading && (item.kind != "item" || item.playable)) {
                        if (item.artwork.isNotEmpty()) Artwork(moduleAsset(s.receiver, m.id, item.artwork), Modifier.fillMaxWidth().height(130.dp))
                        Column(Modifier.padding(12.dp)) {
                            Text(item.title, style = MaterialTheme.typography.titleMedium, maxLines = 3)
                            Text(if (item.kind == "folder") "Folder" else item.subtitle, color = MaterialTheme.colorScheme.onSurfaceVariant)
                            if (!item.playable && item.kind == "item") Text(item.description.ifBlank { "Unavailable" })
                        }
                    }
                }
                item(span = { GridItemSpan(maxLineSpan) }) {
                    Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                        if (s.cursor.previousOffsets.isNotEmpty()) TextButton(onClick = previous, enabled = !s.loading) { Text("Previous page") }
                        if (s.page.hasMore) TextButton(onClick = more, enabled = !s.loading) { Text("Next page") }
                    }
                }
            }
        }
    }
}
@Composable
fun ReceiverSetup(s: ControllerState, value: TextFieldValue, onValue: (TextFieldValue) -> Unit, connect: () -> Unit, modifier: Modifier = Modifier) {
    Column(modifier.fillMaxSize().padding(24.dp), verticalArrangement = Arrangement.Center) {
        Text("ReBox", style = MaterialTheme.typography.headlineLarge)
        Text("Connect to your receiver", modifier = Modifier.padding(vertical = 16.dp))
        WhisperTextField(value, onValue, "Receiver address", maxBytes = 256, onSearch = connect)
        s.error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
        Button(onClick = connect, enabled = !s.loading && value.text.isNotBlank()) { Text(if (s.loading) "Connecting…" else "Connect to receiver") }
    }
}
@Composable
fun SettingsScreen(s: ControllerState, value: TextFieldValue, onValue: (TextFieldValue) -> Unit, connect: () -> Unit,
    test: () -> Unit, modelStatus: String, modules: () -> Unit) {
    LazyColumn(contentPadding = PaddingValues(20.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
        item { Text("Receiver", style = MaterialTheme.typography.titleLarge); Text(s.receiver.orEmpty()) }
        item { WhisperTextField(value, onValue, "Receiver address", maxBytes = 256, onSearch = connect) }
        item { Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) { OutlinedButton(onClick = test) { Text("Test connection") }; Button(onClick = connect, enabled = !s.loading) { Text("Change receiver") } } }
        item { Button(onClick = modules) { Text("Modules") } }
        item { Text("Local speech", style = MaterialTheme.typography.titleLarge); Text(modelStatus); Text("Audio and transcripts stay on this device.") }
        item { Text("ReBox ${BuildConfig.VERSION_NAME} (${BuildConfig.VERSION_CODE})") }
    }
}
@Composable
fun ModuleManager(s: ControllerState, manage: (String, String) -> Unit, settings: (String) -> Unit, install: (String) -> Unit,
    pair: (String) -> Unit, revoke: () -> Unit) {
    var url by rememberSaveable { mutableStateOf("") }
    var code by rememberSaveable { mutableStateOf("") }
    var confirm by remember { mutableStateOf<ModuleDescriptor?>(null) }
    LazyColumn(contentPadding = PaddingValues(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        item {
            Text("Module management", style = MaterialTheme.typography.titleLarge)
            Text(if (s.paired) "Paired with this receiver" else "On the HR54, open Settings → Modules → Pair Android management. Enter its temporary code here.")
            if (!s.paired) {
                OutlinedTextField(code, { code = it.take(8) }, label = { Text("Pairing code") }, singleLine = true)
                Button(onClick = { pair(code) }, enabled = !s.busy && code.length == 8) { Text("Pair management") }
            } else OutlinedButton(onClick = revoke, enabled = !s.busy) { Text("Revoke pairing") }
        }
        items(s.modules, key = { it.id }) { m ->
            val owned = (s.playback.playing && s.playback.source == m.id) || s.system.nativeModule == m.id
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) { Artwork(moduleAsset(s.receiver, m.id), Modifier.size(40.dp)); Text(m.name, style = MaterialTheme.typography.titleMedium) }
                    Text(listOf(if (m.core) "Bundled" else "Third-party", m.version, if (!m.installed) "Not installed" else if (!m.enabled) "Disabled" else if (m.healthy) "Enabled" else "Error").filter { it.isNotEmpty() }.joinToString(" · "))
                    Text(m.error.ifBlank { m.description })
                    if (m.installed) {
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            OutlinedButton(onClick = { manage(m.id, if (m.enabled) "disable" else "enable") }, enabled = s.paired && !s.busy && !owned && m.compatible) { Text(if (m.enabled) "Disable" else "Enable") }
                            TextButton(onClick = { confirm = m }, enabled = s.paired && !s.busy && !owned) { Text("Uninstall") }
                        }
                        if (m.enabled && m.healthy && (m.capabilities.settings)) TextButton(onClick = { settings(m.id) }) { Text("Module settings") }
                    } else if (m.core) Button(onClick = { manage(m.id, "reinstall") }, enabled = s.paired && !s.busy) { Text("Reinstall bundled module") }
                }
            }
        }
        item {
            Text("Add module from URL", style = MaterialTheme.typography.titleMedium)
            Text("Modules run executable code on your receiver. Install packages only from sources you trust.")
            OutlinedTextField(url, { url = truncateUtf8(it, 1024) }, label = { Text("Package URL") }, modifier = Modifier.fillMaxWidth(), singleLine = true)
            Button(onClick = { install(url) }, enabled = s.paired && !s.busy && url.isNotBlank()) { Text("Install module") }
        }
    }
    confirm?.let { m -> AlertDialog(onDismissRequest = { confirm = null }, title = { Text("Uninstall ${m.name}?") }, text = { Text("Its saved data will be kept.") }, confirmButton = { TextButton(onClick = { manage(m.id, "uninstall"); confirm = null }) { Text("Uninstall") } }, dismissButton = { TextButton(onClick = { confirm = null }) { Text("Cancel") } }) }
}
@Composable
fun ModuleSettingsScreen(s: ControllerState, save: (ModuleField, JsonPrimitive) -> Unit, action: (String) -> Unit) {
    LazyColumn(contentPadding = PaddingValues(16.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
        if (!s.paired) item { Text("Pair module management in Settings → Modules to save settings or run actions.") }
        items(s.settings.fields, key = { it.key }) { field ->
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(field.label, style = MaterialTheme.typography.titleMedium)
                when (field.type) {
                    "bool" -> Switch(field.value.booleanOrNull == true, { save(field, JsonPrimitive(it)) }, enabled = s.paired && !s.busy)
                    "choice" -> field.choices.forEach { choice -> Row(verticalAlignment = Alignment.CenterVertically) { RadioButton(field.value == choice.value, { save(field, choice.value) }, enabled = s.paired && !s.busy); Text(choice.label) } }
                    else -> {
                        var edit by rememberSaveable(s.managedModule, field.key, field.value.content) { mutableStateOf(field.value.content) }
                        OutlinedTextField(edit, { edit = truncateUtf8(it, 255) }, singleLine = true, modifier = Modifier.fillMaxWidth())
                        val value = if (field.type == "integer") edit.toLongOrNull()?.let(::JsonPrimitive) else JsonPrimitive(edit)
                        Button(onClick = { value?.let { save(field, it) } }, enabled = value != null && s.paired && !s.busy) { Text("Save ${field.label}") }
                    }
                }
            }
        }
        items(s.settings.actions, key = { it.id }) { Button(onClick = { action(it.id) }, enabled = s.paired && !s.busy) { Text(it.label) } }
        if (s.settings.fields.isEmpty() && s.settings.actions.isEmpty() && !s.loading) item { Text("No settings available") }
    }
}
val previewState = ControllerState(receiver = null, ready = true, connected = true,
    modules = listOf(ModuleDescriptor("example-media", "Example media", installed = true, enabled = true, healthy = true, capabilities = ModuleCapabilities(browse = true, search = true, playback = true)), ModuleDescriptor("example-app", "Example app", kind = "native-app", installed = true, enabled = true, healthy = true, capabilities = ModuleCapabilities(nativeApp = true))),
    activeModule = "example-media", page = MediaPage(listOf(MediaItem("folder", "Collection", "folder"), MediaItem("item", "A media item", playable = true)), 2),
    playback = Playback(playing = true, source = "example-media", title = "A media item", itemId = "item", elapsed = 20.0, transport = Transport(true, true, true, true)))
@Preview(showBackground = true) @Composable fun HomePreview() { Hr54Theme { Surface { HomeScreen(previewState, {}, { _, _ -> }, {}) } } }
