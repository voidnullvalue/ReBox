package com.hr54.controller

import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.core.net.toUri
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.compose.LifecycleResumeEffect
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import com.hr54.controller.data.model.*
import com.hr54.controller.data.repository.ReceiverPreferences
import com.hr54.controller.speech.whisper.WhisperEngine
import com.hr54.controller.ui.*
import com.hr54.controller.ui.components.*

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge(
            statusBarStyle = SystemBarStyle.dark(android.graphics.Color.TRANSPARENT),
            navigationBarStyle = SystemBarStyle.dark(android.graphics.Color.TRANSPARENT),
        )
        val prefs = ReceiverPreferences(applicationContext)
        val whisper = WhisperEngine(applicationContext)
        setContent {
            Hr54Theme {
                CompositionLocalProvider(LocalWhisper provides whisper) {
                    val vm: ControllerViewModel =
                        viewModel(
                            factory =
                                object : ViewModelProvider.Factory {
                                    @Suppress("UNCHECKED_CAST")
                                    override fun <T : ViewModel> create(modelClass: Class<T>): T =
                                        ControllerViewModel(prefs) as T
                                }
                        )
                    ControllerApp(vm, whisper)
                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ControllerApp(vm: ControllerViewModel, whisper: WhisperEngine) {
    val s by vm.state.collectAsStateWithLifecycle()
    val context = LocalContext.current
    var expanded by rememberSaveable { mutableStateOf(false) }
    var detail by remember { mutableStateOf<Item?>(null) }
    var previousDestination by rememberSaveable { mutableStateOf("Home") }
    var receiver by
        rememberSaveable(stateSaver = TextFieldValue.Saver) { mutableStateOf(TextFieldValue("")) }
    var jelly by
        rememberSaveable(stateSaver = TextFieldValue.Saver) { mutableStateOf(TextFieldValue("")) }
    var tv by
        rememberSaveable(stateSaver = TextFieldValue.Saver) { mutableStateOf(TextFieldValue("")) }
    var youtube by
        rememberSaveable(stateSaver = TextFieldValue.Saver) { mutableStateOf(TextFieldValue("")) }
    val snackbar = remember { SnackbarHostState() }
    val haptic = LocalHapticFeedback.current
    val permissionName = "android.permission.ACCESS_LOCAL_NETWORK"
    var lanGranted by remember {
        mutableStateOf(
            Build.VERSION.SDK_INT < 37 ||
                ContextCompat.checkSelfPermission(context, permissionName) ==
                    PackageManager.PERMISSION_GRANTED
        )
    }
    var lanDenied by remember { mutableStateOf(false) }
    val lanRequest =
        rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            lanGranted = granted
            lanDenied = !granted
            if (granted) vm.setForeground(true) else vm.setForeground(false)
        }
    LaunchedEffect(Unit) { if (!lanGranted) lanRequest.launch(permissionName) }
    LifecycleResumeEffect(lanGranted) {
        val allowed =
            Build.VERSION.SDK_INT < 37 ||
                ContextCompat.checkSelfPermission(context, permissionName) ==
                    PackageManager.PERMISSION_GRANTED
        lanGranted = allowed
        vm.setForeground(allowed)
        onPauseOrDispose { vm.setForeground(false) }
    }
    LaunchedEffect(s.playback.playing) { if (!s.playback.playing) expanded = false }
    LaunchedEffect(s.receiver) {
        if (receiver.text.isBlank()) receiver = TextFieldValue(s.receiver.orEmpty())
    }
    LaunchedEffect(s.message) {
        s.message?.let {
            snackbar.showSnackbar(it)
            vm.clearMessage()
        }
    }
    BackHandler(
        expanded ||
            detail != null ||
            s.destination == "Settings" ||
            (s.destination == "Jellyfin" &&
                (s.folders.isNotEmpty() || s.jellyQuery.isNotEmpty())) ||
            s.destination != "Home"
    ) {
        when {
            expanded -> expanded = false
            detail != null -> detail = null
            s.destination == "Settings" -> vm.destination(previousDestination)
            vm.up() -> {
                if (s.jellyQuery.isNotEmpty()) jelly = TextFieldValue()
            }
            else -> vm.destination("Home")
        }
    }
    val actions: (String, Int?) -> Unit = { command, delta -> vm.transport(command, delta = delta) }
    val navigate: (String) -> Unit = { vm.destination(it) }
    Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
        if (!lanGranted) {
            Column(
                Modifier.fillMaxSize().safeDrawingPadding().padding(24.dp),
                verticalArrangement = Arrangement.Center,
            ) {
                Text(
                    "Connect on your local network",
                    style = MaterialTheme.typography.headlineMedium,
                )
                Text(
                    "HR54 needs Android's local network permission to reach your receiver.",
                    modifier = Modifier.padding(vertical = 16.dp),
                )
                Button(
                    onClick = {
                        if (lanDenied)
                            context.startActivity(
                                android.content.Intent(
                                    android.provider.Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                                    "package:${context.packageName}".toUri(),
                                )
                            )
                        else lanRequest.launch(permissionName)
                    }
                ) {
                    Text(if (lanDenied) "App permissions" else "Allow local network")
                }
            }
        } else if (!s.ready)
            ReceiverSetup(
                s,
                receiver,
                { receiver = it },
                { vm.connect(receiver.text) },
                Modifier.safeDrawingPadding(),
            )
        else
            BoxWithConstraints {
                val wide = maxWidth >= 700.dp
                val panel = maxWidth >= 1100.dp && s.playback.playing
                Scaffold(
                    topBar = {
                        Hr54TopBar(
                            if (s.destination == "Home") "HR54" else s.destination,
                            s.connected,
                            vm::retryConnection,
                        ) {
                            previousDestination = s.destination
                            vm.destination("Settings")
                        }
                    },
                    snackbarHost = { SnackbarHost(snackbar) },
                    bottomBar = {
                        if (!wide)
                            Column {
                                if (s.playback.playing)
                                    NowPlayingMiniBar(
                                        s.playback,
                                        s.receiver,
                                        s.busy,
                                        { expanded = true },
                                        actions,
                                    )
                                NavigationBar(
                                    containerColor = MaterialTheme.colorScheme.background,
                                    tonalElevation = 0.dp,
                                ) {
                                    s.capabilities.destinations().forEach { name ->
                                        NavigationBarItem(
                                            selected = s.destination == name,
                                            onClick = { navigate(name) },
                                            icon = { Icon(sourceIcon(name), null) },
                                            label = {
                                                Text(
                                                    if (name == "Live TV") "TV" else name,
                                                    maxLines = 1,
                                                )
                                            },
                                        )
                                    }
                                }
                            }
                    },
                ) { padding ->
                    Row(Modifier.fillMaxSize().padding(padding)) {
                        if (wide)
                            NavigationRail(containerColor = MaterialTheme.colorScheme.background) {
                                s.capabilities.destinations().forEach { name ->
                                    NavigationRailItem(
                                        selected = s.destination == name,
                                        onClick = { navigate(name) },
                                        icon = { Icon(sourceIcon(name), null) },
                                        label = { Text(name) },
                                    )
                                }
                            }
                        Column(Modifier.weight(1f).fillMaxHeight()) {
                            if (s.busy) {
                                LinearProgressIndicator(Modifier.fillMaxWidth())
                                Text(
                                    "Preparing receiver…",
                                    style = MaterialTheme.typography.labelMedium,
                                    modifier =
                                        Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
                                )
                            }
                            if (s.loading && s.destination != "YouTube")
                                LinearProgressIndicator(Modifier.fillMaxWidth())
                            if (s.destination == "Settings")
                                TextButton(onClick = { vm.destination(previousDestination) }) {
                                    Icon(AppIcons.ArrowBack, null)
                                    Text("Back to media")
                                }
                            s.error?.let {
                                InlineError(
                                    it,
                                    if (s.destination == "Settings") ({ vm.connect(receiver.text) })
                                    else vm::retryList,
                                )
                            }
                            Box(Modifier.weight(1f).then(destinationMotion(s.destination))) {
                                when (s.destination) {
                                    "Home" ->
                                        HomeScreen(
                                            s,
                                            navigate,
                                            actions,
                                            { vm.transport("seek", seconds = it) },
                                        )
                                    "Jellyfin" ->
                                        JellyfinScreen(
                                            s,
                                            jelly,
                                            { jelly = it },
                                            { vm.searchJellyfin(jelly.text) },
                                            vm::startAuth,
                                            { if (it.isFolder) vm.folder(it) else detail = it },
                                            vm::moreItems,
                                        )
                                    "Live TV" ->
                                        IptvScreen(
                                            s,
                                            tv,
                                            { tv = it },
                                            { vm.loadChannels(query = tv.text) },
                                            { vm.loadChannels(group = it, query = tv.text) },
                                            {
                                                haptic.performHapticFeedback(
                                                    HapticFeedbackType.LongPress
                                                )
                                                vm.play(it)
                                            },
                                            { vm.loadChannels(more = true) },
                                            wide,
                                        )
                                    "YouTube" ->
                                        YoutubeScreen(
                                            s,
                                            youtube,
                                            { youtube = it },
                                            { vm.searchYoutube(youtube.text) },
                                            {
                                                haptic.performHapticFeedback(
                                                    HapticFeedbackType.LongPress
                                                )
                                                vm.play(it)
                                            },
                                            { vm.searchYoutube(more = true) },
                                        )
                                    "Cameras" ->
                                        CamerasScreen(
                                            s,
                                            {
                                                haptic.performHapticFeedback(
                                                    HapticFeedbackType.LongPress
                                                )
                                                vm.play(it)
                                            },
                                        )
                                    "Settings" ->
                                        SettingsScreen(
                                            s,
                                            receiver,
                                            { receiver = it },
                                            { vm.connect(receiver.text) },
                                            vm::testConnection,
                                            whisper.status,
                                            vm::logout,
                                        )
                                }
                            }
                            if (wide && !panel && s.playback.playing)
                                NowPlayingMiniBar(
                                    s.playback,
                                    s.receiver,
                                    s.busy,
                                    { expanded = true },
                                    actions,
                                )
                        }
                        if (panel)
                            Surface(
                                Modifier.width(320.dp).fillMaxHeight(),
                                color = MaterialTheme.colorScheme.surface,
                            ) {
                                NowPlayingContent(
                                    s.playback,
                                    s.receiver,
                                    s.playback.duration ?: s.knownDurations[s.playback.itemId],
                                    s.busy,
                                    actions,
                                    { vm.transport("seek", seconds = it) },
                                )
                            }
                    }
                }
            }
    }
    if (expanded && s.playback.playing)
        NowPlayingSheet(
            s.playback,
            s.receiver,
            s.playback.duration ?: s.knownDurations[s.playback.itemId],
            s.busy,
            { expanded = false },
            actions,
            { vm.transport("seek", seconds = it) },
        )
    detail?.let { item ->
        ModalBottomSheet(onDismissRequest = { detail = null }) {
            Column(
                Modifier.verticalScroll(rememberScrollState()).padding(20.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Row(horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                    Artwork(
                        "${s.receiver}/art/${item.id}.jpg",
                        Modifier.width(110.dp).aspectRatio(2f / 3),
                    )
                    Column {
                        Text(item.name, style = MaterialTheme.typography.headlineSmall)
                        Text(
                            listOfNotNull(
                                    item.year?.toString(),
                                    item.seconds?.let(::timeLabel),
                                    item.type,
                                )
                                .joinToString(" · "),
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
                Text(item.overview.ifBlank { "No description available." })
                Button(
                    onClick = {
                        haptic.performHapticFeedback(HapticFeedbackType.LongPress)
                        vm.play(item)
                        detail = null
                    },
                    enabled = item.playable && !s.busy,
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    Text("Play on HR54")
                }
                Spacer(Modifier.height(20.dp))
            }
        }
    }

}
