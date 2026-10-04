package com.hr54.controller.ui.components

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.provider.Settings
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.TextRange
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.core.net.toUri
import com.hr54.controller.data.model.*
import com.hr54.controller.speech.whisper.*
import kotlinx.coroutines.CancellationException

val LocalWhisper = staticCompositionLocalOf<WhisperEngine?> { null }

@Composable
fun WhisperTextField(
    value: TextFieldValue,
    onValueChange: (TextFieldValue) -> Unit,
    label: String,
    modifier: Modifier = Modifier,
    maxBytes: Int = 64,
    onSearch: () -> Unit = {},
) {
    val context = LocalContext.current
    val engine = LocalWhisper.current
    var capture by remember { mutableStateOf(false) }
    var denial by remember { mutableStateOf(false) }
    var permanentlyDenied by remember { mutableStateOf(false) }
    val latestValue by rememberUpdatedState(value)
    val latestChange by rememberUpdatedState(onValueChange)
    val permission =
        rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            if (granted) capture = true
            else {
                permanentlyDenied =
                    (context as? Activity)?.shouldShowRequestPermissionRationale(
                        Manifest.permission.RECORD_AUDIO
                    ) == false
                denial = true
            }
        }
    OutlinedTextField(
        value = value,
        onValueChange = { candidate ->
            val text = truncateUtf8(candidate.text, maxBytes)
            onValueChange(
                candidate.copy(
                    text = text,
                    selection =
                        TextRange(
                            candidate.selection.start.coerceAtMost(text.length),
                            candidate.selection.end.coerceAtMost(text.length),
                        ),
                )
            )
        },
        label = { Text(label) },
        singleLine = true,
        modifier = modifier,
        shape = MaterialTheme.shapes.medium,
        keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
        keyboardActions = KeyboardActions(onSearch = { onSearch() }),
        trailingIcon = {
            IconButton(
                onClick = {
                    if (engine == null) {
                        denial = true
                    } else if (
                        ContextCompat.checkSelfPermission(
                            context,
                            Manifest.permission.RECORD_AUDIO,
                        ) == PackageManager.PERMISSION_GRANTED
                    )
                        capture = true
                    else if (permanentlyDenied) denial = true
                    else permission.launch(Manifest.permission.RECORD_AUDIO)
                }
            ) {
                Icon(AppIcons.Mic, "Dictate text")
            }
        },
    )
    if (capture && engine != null)
        WhisperCaptureSheet(
            engine,
            onDismiss = { capture = false },
            onTranscript = { transcript ->
                val old = latestValue
                val inserted =
                    insertTranscript(
                        old.text,
                        old.selection.start,
                        old.selection.end,
                        transcript,
                        maxBytes,
                    )
                latestChange(TextFieldValue(inserted.text, TextRange(inserted.cursor)))
                capture = false
            },
        )
    if (denial)
        AlertDialog(
            onDismissRequest = { denial = false },
            title = { Text("Voice input unavailable") },
            text = {
                Text(
                    if (engine == null) "Manual text entry remains available."
                    else "Allow microphone access to dictate text. You can always type instead."
                )
            },
            confirmButton = { TextButton(onClick = { denial = false }) { Text("OK") } },
            dismissButton = {
                if (permanentlyDenied)
                    TextButton(
                        onClick = {
                            context.startActivity(
                                Intent(
                                    Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                                    "package:${context.packageName}".toUri(),
                                )
                            )
                            denial = false
                        }
                    ) {
                        Text("App permissions")
                    }
            },
        )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun WhisperCaptureSheet(
    engine: WhisperEngine,
    onDismiss: () -> Unit,
    onTranscript: (String) -> Unit,
) {
    val lifecycle = androidx.lifecycle.compose.LocalLifecycleOwner.current.lifecycle
    val latestDismiss by rememberUpdatedState(onDismiss)
    DisposableEffect(lifecycle) {
        val observer =
            androidx.lifecycle.LifecycleEventObserver { _, event ->
                if (event == androidx.lifecycle.Lifecycle.Event.ON_PAUSE) latestDismiss()
            }
        lifecycle.addObserver(observer)
        onDispose { lifecycle.removeObserver(observer) }
    }
    val capture = remember { AudioCapture(engine) }
    val state by capture.state.collectAsState()
    var error by remember { mutableStateOf<String?>(null) }
    LaunchedEffect(capture) {
        try {
            val text = capture.run()
            if (text.isBlank()) error = "No speech detected. Try again or type your query."
            else onTranscript(text)
        } catch (e: Exception) {
            if (e is CancellationException) throw e
            error = e.message ?: "Local transcription failed"
        }
    }
    DisposableEffect(capture) { onDispose { capture.stop() } }
    ModalBottomSheet(onDismissRequest = onDismiss) {
        WhisperCaptureContent(state, error, onDismiss, { capture.stop() })
    }
}

@Composable
fun WhisperCaptureContent(
    state: CaptureState,
    error: String?,
    cancel: () -> Unit,
    stop: () -> Unit,
) {
    Column(
        Modifier.fillMaxWidth().padding(24.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Text(
            error?.let { "Voice input unavailable" } ?: state.phase,
            style = MaterialTheme.typography.headlineSmall,
        )
        Text(
            "Local speech · audio stays on your phone",
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        if (error != null) Text(error, color = MaterialTheme.colorScheme.error)
        else if (state.phase == "Listening…") {
            VoiceLevel(state.level)
            Text("${state.seconds}s / 12s", style = MaterialTheme.typography.labelMedium)
        } else LinearProgressIndicator(Modifier.fillMaxWidth())
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
            TextButton(onClick = cancel) { Text("Cancel") }
            if (error == null && state.phase == "Listening…")
                Button(onClick = stop) { Text("Stop") }
        }
        Spacer(Modifier.height(16.dp))
    }
}

@Preview(showBackground = true, widthDp = 412)
@Composable
fun WhisperCapturePreview() {
    Hr54Theme {
        Surface { WhisperCaptureContent(CaptureState(level = .45f, seconds = 3), null, {}, {}) }
    }
}
