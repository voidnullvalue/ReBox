package com.hr54.controller.speech.whisper

import android.content.Context
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import java.io.File
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.sqrt
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.*
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

class WhisperEngine(private val context: Context) {
    companion object {
        const val MODEL = "ggml-base.en-q5_1.bin"
    }

    private external fun transcribeNative(path: String, audio: FloatArray): String

    private val mutex = Mutex()
    val status: String
        get() =
            if (File(context.filesDir, MODEL).exists()) "Ready · private model extracted"
            else "Bundled · extracts privately on first use"

    suspend fun transcribe(audio: FloatArray): String = mutex.withLock {
        withContext(Dispatchers.Default) {
            try {
                System.loadLibrary("hr54-whisper")
            } catch (e: LinkageError) {
                throw IllegalStateException(
                    "Local speech library unavailable. You can still type.",
                    e,
                )
            }
            val model = File(context.filesDir, MODEL)
            if (!model.exists()) {
                val temp = File(context.filesDir, "$MODEL.tmp")
                context.assets.open(MODEL).use { input ->
                    temp.outputStream().use { input.copyTo(it) }
                }
                check(temp.renameTo(model)) { "Unable to prepare bundled model" }
            }
            transcribeNative(model.absolutePath, audio).trim()
        }
    }
}

data class CaptureState(
    val phase: String = "Listening…",
    val level: Float = 0f,
    val seconds: Int = 0,
    val error: String? = null,
)

class AudioCapture(private val engine: WhisperEngine) {
    private val stop = AtomicBoolean(false)
    private val mutable = MutableStateFlow(CaptureState())
    val state = mutable.asStateFlow()

    fun stop() {
        stop.set(true)
    }

    @android.annotation.SuppressLint("MissingPermission")
    suspend fun run(): String =
        withContext(Dispatchers.IO) {
            stop.set(false)
            val minimum =
                AudioRecord.getMinBufferSize(
                    16000,
                    AudioFormat.CHANNEL_IN_MONO,
                    AudioFormat.ENCODING_PCM_16BIT,
                )
            check(minimum > 0) { "16 kHz audio capture is unavailable" }
            val recorder =
                AudioRecord(
                    MediaRecorder.AudioSource.VOICE_RECOGNITION,
                    16000,
                    AudioFormat.CHANNEL_IN_MONO,
                    AudioFormat.ENCODING_PCM_16BIT,
                    maxOf(minimum, 4096),
                )
            val samples = FloatArray(16000 * 12)
            var count = 0
            var silent = 0
            var heard = false
            val buffer = ShortArray(1024)
            try {
                check(recorder.state == AudioRecord.STATE_INITIALIZED) { "Microphone unavailable" }
                recorder.startRecording()
                while (isActive && !stop.get() && count < samples.size) {
                    val n =
                        recorder.read(
                            buffer,
                            0,
                            minOf(buffer.size, samples.size - count),
                            AudioRecord.READ_BLOCKING,
                        )
                    check(n > 0) { "Microphone capture failed ($n)" }
                    var sum = 0.0
                    repeat(n) {
                        val v = buffer[it] / 32768f
                        samples[count + it] = v
                        sum += v * v
                    }
                    count += n
                    val rms = sqrt(sum / n).toFloat()
                    if (rms > 0.012f) {
                        heard = true
                        silent = 0
                    } else silent += n
                    mutable.value =
                        CaptureState(level = (rms * 12).coerceIn(0f, 1f), seconds = count / 16000)
                    if (heard && count > 16000 && silent > 16000 * 1.4) break
                }
            } finally {
                if (recorder.recordingState == AudioRecord.RECORDSTATE_RECORDING) recorder.stop()
                recorder.release()
            }
            ensureActive()
            if (count < 3200 || !heard) return@withContext ""
            mutable.value = CaptureState("Transcribing…", seconds = count / 16000)
            engine.transcribe(samples.copyOf(count))
        }
}
