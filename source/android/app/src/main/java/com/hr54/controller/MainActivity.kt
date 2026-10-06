package com.hr54.controller

import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.*
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.core.net.toUri
import androidx.lifecycle.*
import androidx.lifecycle.compose.*
import androidx.lifecycle.viewmodel.compose.viewModel
import com.hr54.controller.data.model.*
import com.hr54.controller.data.repository.*
import com.hr54.controller.speech.whisper.WhisperEngine
import com.hr54.controller.ui.*
import com.hr54.controller.ui.components.*

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge(statusBarStyle = SystemBarStyle.dark(android.graphics.Color.TRANSPARENT), navigationBarStyle = SystemBarStyle.dark(android.graphics.Color.TRANSPARENT))
        val prefs = ReceiverPreferences(applicationContext)
        val tokens = PrivateManagementTokens(applicationContext)
        val whisper = WhisperEngine(applicationContext)
        setContent { Hr54Theme { CompositionLocalProvider(LocalWhisper provides whisper) {
            val vm: ControllerViewModel = viewModel(factory = object : ViewModelProvider.Factory {
                @Suppress("UNCHECKED_CAST") override fun <T : ViewModel> create(modelClass: Class<T>): T = ControllerViewModel(prefs, tokens = tokens) as T
            })
            ControllerApp(vm, whisper)
        } } }
    }
}
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ControllerApp(vm: ControllerViewModel, whisper: WhisperEngine) {
    val s by vm.state.collectAsStateWithLifecycle()
    val context = LocalContext.current
    var receiver by rememberSaveable(stateSaver = TextFieldValue.Saver) { mutableStateOf(TextFieldValue()) }
    var query by rememberSaveable(s.activeModule, stateSaver = TextFieldValue.Saver) { mutableStateOf(TextFieldValue()) }
    var detail by remember { mutableStateOf<MediaItem?>(null) }
    var expanded by rememberSaveable { mutableStateOf(false) }
    val snackbar = remember { SnackbarHostState() }
    val permissionName = "android.permission.ACCESS_LOCAL_NETWORK"
    var lanGranted by remember { mutableStateOf(Build.VERSION.SDK_INT < 37 || ContextCompat.checkSelfPermission(context, permissionName) == PackageManager.PERMISSION_GRANTED) }
    var lanDenied by remember { mutableStateOf(false) }
    val lanRequest = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted -> lanGranted = granted; lanDenied = !granted; vm.setForeground(granted) }
    LaunchedEffect(Unit) { if (!lanGranted) lanRequest.launch(permissionName) }
    LifecycleResumeEffect(lanGranted) {
        val allowed = Build.VERSION.SDK_INT < 37 || ContextCompat.checkSelfPermission(context, permissionName) == PackageManager.PERMISSION_GRANTED
        lanGranted = allowed; vm.setForeground(allowed)
        onPauseOrDispose { vm.setForeground(false) }
    }
    LaunchedEffect(s.receiver) { receiver = TextFieldValue(s.receiver.orEmpty()) }
    LaunchedEffect(s.cursor.query) { query = TextFieldValue(s.cursor.query) }
    LaunchedEffect(s.destination, s.activeModule) { detail = null }
    LaunchedEffect(s.playback.playing) { if (!s.playback.playing) expanded = false }
    LaunchedEffect(s.message) { s.message?.let { snackbar.showSnackbar(it); vm.clearMessage() } }
    BackHandler(expanded || detail != null || s.destination != "Home") {
        when { expanded -> expanded = false; detail != null -> detail = null; vm.up() -> Unit; s.destination == "Modules" -> vm.destination("Settings"); else -> vm.destination("Home") }
    }
    val actions: (String, Int?) -> Unit = { command, delta -> vm.transport(command, delta = delta) }
    CompositionLocalProvider(LocalModules provides s.modules, LocalMediaPage provides (s.activeModule to s.page.items)) {
        Surface(Modifier.fillMaxSize()) {
            if (!lanGranted) Column(Modifier.fillMaxSize().safeDrawingPadding().padding(24.dp), verticalArrangement = Arrangement.Center) {
                Text("Connect on your local network", style = MaterialTheme.typography.headlineMedium)
                Text("Allow local network access to reach your receiver.", modifier = Modifier.padding(vertical = 16.dp))
                Button(onClick = { if (lanDenied) context.startActivity(android.content.Intent(android.provider.Settings.ACTION_APPLICATION_DETAILS_SETTINGS, "package:${context.packageName}".toUri())) else lanRequest.launch(permissionName) }) { Text(if (lanDenied) "App permissions" else "Allow local network") }
            }
            else if (!s.ready) ReceiverSetup(s, receiver, { receiver = it }, { vm.connect(receiver.text) }, Modifier.safeDrawingPadding())
            else Scaffold(
                topBar = { Hr54TopBar(when (s.destination) { "Home" -> "ReBox"; "Module" -> s.module(s.activeModule)?.name ?: "Module"; "ModuleSettings" -> s.module(s.managedModule)?.name ?: "Module settings"; else -> s.destination }, s.connected, vm::retryConnection, { vm.destination("Settings") }) },
                snackbarHost = { SnackbarHost(snackbar) },
                bottomBar = {
                    Column {
                        if (s.playback.playing) NowPlayingMiniBar(s.playback, s.receiver, s.busy, { expanded = true }, actions)
                        NavigationBar {
                            NavigationBarItem(s.destination == "Home" || s.destination == "Module", { vm.destination("Home") }, { Icon(AppIcons.Home, null) }, label = { Text("Home") })
                            NavigationBarItem(s.destination in listOf("Settings", "Modules", "ModuleSettings"), { vm.destination("Settings") }, { Icon(AppIcons.Settings, null) }, label = { Text("Settings") })
                        }
                    }
                },
            ) { padding -> Column(Modifier.padding(padding).fillMaxSize()) {
                if (s.destination != "Home") TextButton(onClick = { if (!vm.up()) vm.destination(if (s.destination == "Modules") "Settings" else "Home") }) { Icon(AppIcons.ArrowBack, null); Text("Back") }
                if (s.loading || s.busy) LinearProgressIndicator(Modifier.fillMaxWidth())
                s.error?.let { InlineError(it, vm::retryList) }
                Box(Modifier.weight(1f).then(destinationMotion(s.destination))) {
                    when (s.destination) {
                        "Home" -> HomeScreen(s, vm::openModule, actions, { vm.transport("seek", seconds = it) })
                        "Module" -> ModuleScreen(s, query, { query = it }, { vm.search(query.text) }, { if (it.kind == "item") detail = it else vm.select(it) }, vm::moreItems, vm::previousPage, { s.activeModule?.let(vm::moduleSettings) }, { start -> s.activeModule?.let { vm.nativeApp(it, start) } })
                        "Settings" -> SettingsScreen(s, receiver, { receiver = it }, { vm.connect(receiver.text) }, vm::testConnection, whisper.status, { vm.destination("Modules") })
                        "Modules" -> ModuleManager(s, vm::manage, vm::moduleSettings, vm::install, vm::pair, vm::revokePairing)
                        "ModuleSettings" -> ModuleSettingsScreen(s, { field, value -> s.managedModule?.let { vm.saveField(it, field, value) } }, { action -> s.managedModule?.let { vm.moduleAction(it, action) } })
                    }
                }
            } }
        }
        if (expanded && s.playback.playing) NowPlayingSheet(s.playback, s.receiver, s.playback.duration, s.busy, { expanded = false }, actions, { vm.transport("seek", seconds = it) })
        detail?.let { item -> ModalBottomSheet(onDismissRequest = { detail = null }) {
            Column(Modifier.verticalScroll(rememberScrollState()).padding(20.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                s.activeModule?.let { id -> if (item.artwork.isNotEmpty()) Artwork(moduleAsset(s.receiver, id, item.artwork), Modifier.height(160.dp).fillMaxWidth()) }
                Text(item.title, style = MaterialTheme.typography.headlineSmall)
                if (item.subtitle.isNotEmpty()) Text(item.subtitle)
                Text(item.description.ifBlank { "No description available." })
                Button(onClick = { vm.play(item); detail = null }, enabled = item.playable && !s.busy) { Text(if (item.resume > 0) "Resume on HR54" else "Play on HR54") }
                Spacer(Modifier.height(20.dp))
            }
        } }
        s.actionResult?.let { result -> AlertDialog(onDismissRequest = vm::dismissAction, title = { Text("Module action") }, text = { Column { Text(result.message.ifBlank { if (result.pending) "Waiting for approval…" else "Action complete" }); result.code?.let { Text(it, style = MaterialTheme.typography.headlineLarge) } } }, confirmButton = { TextButton(onClick = vm::dismissAction) { Text("Close") } }) }
    }
}
