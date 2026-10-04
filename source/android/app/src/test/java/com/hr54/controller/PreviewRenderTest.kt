package com.hr54.controller

import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalInspectionMode
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.v2.createComposeRule
import androidx.compose.ui.text.input.TextFieldValue
import com.hr54.controller.speech.whisper.CaptureState
import com.hr54.controller.ui.*
import com.hr54.controller.ui.components.*
import java.awt.image.BufferedImage
import java.io.File
import javax.imageio.ImageIO
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [36], qualifiers = "w412dp-h850dp-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class PreviewRenderTest {
    @get:Rule val compose = createComposeRule()

    private fun render(name: String, content: @Composable () -> Unit) {
        compose.setContent {
            Hr54Theme {
                CompositionLocalProvider(LocalInspectionMode provides true) {
                    Surface(Modifier.fillMaxSize().testTag("preview")) { content() }
                }
            }
        }
        compose.waitForIdle()
        lateinit var output: BufferedImage
        compose.runOnIdle {
            val activity =
                androidx.test.runner.lifecycle.ActivityLifecycleMonitorRegistry.getInstance()
                    .getActivitiesInStage(androidx.test.runner.lifecycle.Stage.RESUMED)
                    .single()
            val view = activity.window.decorView
            val bitmap =
                android.graphics.Bitmap.createBitmap(
                    view.width,
                    view.height,
                    android.graphics.Bitmap.Config.ARGB_8888,
                )
            view.draw(android.graphics.Canvas(bitmap))
            val pixels = IntArray(bitmap.width * bitmap.height)
            bitmap.getPixels(pixels, 0, bitmap.width, 0, 0, bitmap.width, bitmap.height)
            output = BufferedImage(bitmap.width, bitmap.height, BufferedImage.TYPE_INT_ARGB)
            output.setRGB(0, 0, bitmap.width, bitmap.height, pixels, 0, bitmap.width)
            bitmap.recycle()
        }
        val dir = File(System.getProperty("hr54.previewDir", "build/previews")!!)
        dir.mkdirs()
        ImageIO.write(output, "png", File(dir, "$name.png"))
    }

    @Test
    fun home() =
        render("home") {
            Column {
                Hr54TopBar("HR54", true, {}, {})
                HomeScreen(previewState, {}, { _, _ -> }, {}, Modifier.weight(1f))
            }
        }

    @Test
    fun jellyfin() =
        render("jellyfin") {
            Column {
                Hr54TopBar("Jellyfin", true, {}, {})
                JellyfinScreen(previewState, TextFieldValue(), {}, {}, {}, {}, {})
            }
        }

    @Test
    fun iptv() =
        render("iptv") {
            Column {
                Hr54TopBar("Live TV", true, {}, {})
                IptvScreen(previewState, TextFieldValue(), {}, {}, {}, {}, {}, false)
            }
        }

    @Test
    fun youtube() =
        render("youtube") {
            Column {
                Hr54TopBar("YouTube", true, {}, {})
                YoutubeScreen(previewState, TextFieldValue("wildlife"), {}, {}, {}, {})
            }
        }

    @Test
    fun cameras() =
        render("cameras") {
            Column {
                Hr54TopBar("Cameras", true, {}, {})
                CamerasScreen(previewState, {})
            }
        }

    @Test
    fun nowPlaying() =
        render("now-playing") {
            NowPlayingContent(previewState.playback, null, 7020.0, false, { _, _ -> }, {})
        }

    @Test
    fun whisper() =
        render("whisper-capture") {
            WhisperCaptureContent(CaptureState(level = .45f, seconds = 3), null, {}, {})
        }
}
